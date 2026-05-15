#pragma once

#include "utils/logger.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vms::utils {

// Public snapshot of runner state for ops/operator visibility. All fields are
// monotonic counters (submitted/dropped/peak) except current_queue_depth and
// worker_count which are point-in-time. Returned by stats() — values are
// loaded with relaxed atomics so a snapshot may be inconsistent across fields
// for a few ns but is wait-free w.r.t. the producer hot path. Field order
// matters: getter packs them with no locks.
struct BackgroundJobRunnerStats {
    std::string name;
    std::size_t worker_count{0};
    std::size_t max_queue_size{0};
    std::size_t current_queue_depth{0};
    std::uint64_t submitted_total{0};
    std::uint64_t dropped_total{0};
    std::uint64_t peak_queue_depth{0};
    bool stopping{false};
};

class BackgroundJobRunner {
public:
    BackgroundJobRunner(std::string name,
                        std::size_t worker_count = 1,
                        std::size_t max_queue_size = 128)
        : name_(std::move(name)),
          max_queue_size_(max_queue_size) {
        worker_count = std::max<std::size_t>(1, worker_count);
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back(&BackgroundJobRunner::workerLoop, this);
        }
    }

    ~BackgroundJobRunner() {
        shutdown();
    }

    BackgroundJobRunner(const BackgroundJobRunner&) = delete;
    BackgroundJobRunner& operator=(const BackgroundJobRunner&) = delete;

    bool submit(std::function<void()> job) {
        if (!job) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            // Dropped-during-shutdown still counts as a drop so operators can
            // see "we lost N alerts because the producer raced shutdown".
            dropped_total_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (jobs_.size() >= max_queue_size_) {
            dropped_total_.fetch_add(1, std::memory_order_relaxed);
            LOG_THROTTLED_WARN(5000,
                               "BackgroundJobRunner[{}] queue full ({}), dropping job",
                               name_, jobs_.size());
            return false;
        }

        jobs_.push(std::move(job));
        const std::size_t depth = jobs_.size();
        submitted_total_.fetch_add(1, std::memory_order_relaxed);
        // Peak depth: best-effort max via CAS loop. Under the queue mutex so a
        // simple compare-store would also be fine, but keeping it atomic-only
        // keeps stats() lock-free.
        std::uint64_t prev_peak = peak_queue_depth_.load(std::memory_order_relaxed);
        while (depth > prev_peak &&
               !peak_queue_depth_.compare_exchange_weak(prev_peak, depth,
                                                        std::memory_order_relaxed)) {
            // retry with refreshed prev_peak
        }
        cv_.notify_one();
        return true;
    }

    BackgroundJobRunnerStats stats() const {
        BackgroundJobRunnerStats s;
        s.name                = name_;
        s.worker_count        = workers_.size();
        s.max_queue_size      = max_queue_size_;
        // current_queue_depth needs the mutex for an accurate read, but
        // operators just want a coarse number; a non-blocking try_lock
        // approximation keeps stats() never-contending the producer path.
        // On lock contention we report a -1-flavoured sentinel (max size_t)
        // is too confusing, so we just skip and leave 0 — the next poll
        // will likely succeed.
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        if (lk.owns_lock()) {
            s.current_queue_depth = jobs_.size();
        }
        s.submitted_total     = submitted_total_.load(std::memory_order_relaxed);
        s.dropped_total       = dropped_total_.load(std::memory_order_relaxed);
        s.peak_queue_depth    = peak_queue_depth_.load(std::memory_order_relaxed);
        s.stopping            = stopping_.load(std::memory_order_relaxed);
        return s;
    }

    void shutdown() {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true)) {
            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            workers_.clear();
            return;
        }

        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() {
                    return stopping_.load(std::memory_order_acquire) || !jobs_.empty();
                });

                if (jobs_.empty()) {
                    if (stopping_.load(std::memory_order_acquire)) {
                        return;
                    }
                    continue;
                }

                job = std::move(jobs_.front());
                jobs_.pop();
            }

            try {
                job();
            } catch (const std::exception& e) {
                LOG_ERROR("BackgroundJobRunner[{}] job failed: {}", name_, e.what());
            } catch (...) {
                LOG_ERROR("BackgroundJobRunner[{}] job failed with unknown exception", name_);
            }
        }
    }

    const std::string name_;
    const std::size_t max_queue_size_;
    std::atomic<bool> stopping_{false};
    // mutex is `mutable` so stats() can take it read-only via try_to_lock.
    // (Mutex itself isn't const, but lock ops never modify visible state.)
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;

    // Stats counters — monotonic, atomic, relaxed memory order. Producer
    // increments inside submit() (under mutex_); consumer reads via stats()
    // wait-free. Operators consume via /api/rules/engine/stats.
    std::atomic<std::uint64_t> submitted_total_{0};
    std::atomic<std::uint64_t> dropped_total_{0};
    std::atomic<std::uint64_t> peak_queue_depth_{0};
};

} // namespace vms::utils
