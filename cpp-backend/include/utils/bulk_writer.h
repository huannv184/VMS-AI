// ==============================================================
// File: include/utils/bulk_writer.h
// Generic batched writer (header-only). Mirrors the pattern of
// DbManager::EventBatchWriter — dedicated thread, deque + cv,
// transaction-wrapped flush, drop-oldest backpressure.
//
// Caller provides a flush function: bool(QSqlDatabase&, const std::deque<RowT>&).
// The BulkWriter owns the thread; connection is acquired lazily from
// DbManager::getThreadConnection() inside the writer thread (Qt's
// connection registry is thread-local by name).
//
// Lifecycle:
//   BulkWriter<Row> w("attendance_writer", flushFn);
//   w.start();
//   ...  w.enqueue(row);
//   w.stop();        // MUST be called before DbManager::close()
// ==============================================================

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <QSqlDatabase>
#include "database/db_manager.h"
#include "utils/logger.h"

namespace vms::utils {

template <typename RowT>
class BulkWriter {
public:
    using FlushFn = std::function<bool(QSqlDatabase& db, std::deque<RowT>& batch)>;

    struct Options {
        std::size_t batch_size      = 50;
        int         interval_ms     = 100;
        std::size_t max_queue       = 10000;
        int         backoff_ms      = 1000;  // sleep on flush failure
    };

    BulkWriter(std::string name, FlushFn flush, Options opts = {})
        : name_(std::move(name)),
          flush_(std::move(flush)),
          opts_(opts) {}

    ~BulkWriter() { stop(); }

    BulkWriter(const BulkWriter&) = delete;
    BulkWriter& operator=(const BulkWriter&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        accepting_.store(true, std::memory_order_release);
        thread_ = std::thread(&BulkWriter::loop, this);
        LOG_INFO("BulkWriter[{}]: started (batch={}, interval={}ms, max_queue={})",
                 name_, opts_.batch_size, opts_.interval_ms, opts_.max_queue);
    }

    void stop() {
        accepting_.store(false, std::memory_order_release);
        if (!running_.exchange(false)) return;
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();

        // Final drain on caller thread (uses caller's QSqlDatabase via getThreadConnection)
        std::deque<RowT> remaining;
        {
            std::lock_guard<std::mutex> lk(mu_);
            remaining.swap(queue_);
        }
        if (!remaining.empty()) {
            LOG_INFO("BulkWriter[{}]: draining {} rows on shutdown",
                     name_, remaining.size());
            try {
                auto db = vms::database::DbManager::getInstance().getThreadConnection();
                if (db.isValid() && db.isOpen()) {
                    flush_(db, remaining);
                } else {
                    LOG_WARN("BulkWriter[{}]: shutdown drain dropped {} rows (no DB)",
                             name_, remaining.size());
                }
            } catch (const std::exception& e) {
                LOG_ERROR("BulkWriter[{}]: shutdown drain exception: {}", name_, e.what());
            } catch (...) {
                LOG_ERROR("BulkWriter[{}]: shutdown drain unknown exception", name_);
            }
        }
        LOG_INFO("BulkWriter[{}]: stopped", name_);
    }

    // enqueue is thread-safe and lock-cheap. Drops oldest if over capacity.
    void enqueue(RowT row) {
        if (!accepting_.load(std::memory_order_acquire)) return;
        bool notify = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (queue_.size() >= opts_.max_queue) {
                queue_.pop_front();  // drop oldest, prefer recency
                drop_count_.fetch_add(1, std::memory_order_relaxed);
            }
            queue_.push_back(std::move(row));
            notify = (queue_.size() >= opts_.batch_size);
        }
        if (notify) cv_.notify_one();
    }

    std::size_t queueSize() const {
        std::lock_guard<std::mutex> lk(mu_);
        return queue_.size();
    }

    std::uint64_t droppedCount() const {
        return drop_count_.load(std::memory_order_relaxed);
    }

private:
    void loop() {
        LOG_INFO("BulkWriter[{}]: thread started", name_);
        // Connection is acquired lazily and re-fetched if invalid.
        // DbManager keeps thread-local connection by std::this_thread::get_id(),
        // so each BulkWriter thread gets its own QSqlDatabase.
        while (running_.load()) {
            try {
                std::deque<RowT> batch;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait_for(lk,
                        std::chrono::milliseconds(opts_.interval_ms),
                        [this] {
                            return !running_.load() || queue_.size() >= opts_.batch_size;
                        });
                    if (queue_.empty()) continue;
                    batch.swap(queue_);
                }

                auto db = vms::database::DbManager::getInstance().getThreadConnection();
                if (!db.isValid() || !db.isOpen()) {
                    // Re-enqueue and back off
                    std::lock_guard<std::mutex> lk(mu_);
                    for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
                        queue_.push_front(std::move(*it));
                    }
                    LOG_THROTTLED_WARN(5000,
                        "BulkWriter[{}]: no DB connection — back off {}ms",
                        name_, opts_.backoff_ms);
                    std::this_thread::sleep_for(std::chrono::milliseconds(opts_.backoff_ms));
                    continue;
                }

                if (!flush_(db, batch)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
                        queue_.push_front(std::move(*it));
                    }
                    LOG_WARN("BulkWriter[{}]: re-enqueued {} rows after flush failure",
                             name_, batch.size());
                    std::this_thread::sleep_for(std::chrono::milliseconds(opts_.backoff_ms));
                }
            } catch (const std::exception& e) {
                LOG_ERROR("BulkWriter[{}]: exception in loop: {}", name_, e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(opts_.backoff_ms));
            } catch (...) {
                LOG_ERROR("BulkWriter[{}]: unknown exception in loop", name_);
                std::this_thread::sleep_for(std::chrono::milliseconds(opts_.backoff_ms));
            }
        }
        LOG_INFO("BulkWriter[{}]: thread exiting", name_);
    }

    std::string             name_;
    FlushFn                 flush_;
    Options                 opts_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::deque<RowT>        queue_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       accepting_{false};
    std::atomic<uint64_t>   drop_count_{0};
    std::thread             thread_;
};

}  // namespace vms::utils
