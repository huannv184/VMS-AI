#pragma once
#include <vector>
#include <mutex>

namespace vms {

template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t cap) : capacity_(cap) {}

    void push(const T& v) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (buf_.size() >= capacity_) buf_.erase(buf_.begin());
        buf_.push_back(v);
    }

    bool pop(T& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (buf_.empty()) return false;
        out = buf_.front();
        buf_.erase(buf_.begin());
        return true;
    }

private:
    size_t capacity_;
    std::vector<T> buf_;
    std::mutex mtx_;
};

}
