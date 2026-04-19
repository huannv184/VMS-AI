#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

namespace vms {
namespace utils {

template <typename T>
class SafeQueue {
public:
    SafeQueue() : stop_(false) {}

    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(value));
        cond_.notify_one();
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return !queue_.empty() || stop_; });
        
        if (queue_.empty() && stop_) {
            return false; // Queue stopped and empty
        }
        
        if (!queue_.empty()) {
            value = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        
        return false;
    }
    
    // Non-blocking pop
    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        cond_.notify_all();
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool stop_;
};

} // namespace utils
} // namespace vms
