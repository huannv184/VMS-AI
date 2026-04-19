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
            return false;
        }
        if (jobs_.size() >= max_queue_size_) {
            LOG_THROTTLED_WARN(5000,
                               "BackgroundJobRunner[{}] queue full ({}), dropping job",
                               name_, jobs_.size());
            return false;
        }

        jobs_.push(std::move(job));
        cv_.notify_one();
        return true;
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
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
};

} // namespace vms::utils
