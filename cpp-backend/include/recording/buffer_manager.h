#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <iostream>

namespace vms {
namespace recording {

struct VideoPacket {
    std::vector<char> data;
    long long timestamp; // ms
    bool is_keyframe;
};

class BufferManager {
public:
    BufferManager(size_t max_duration_ms = 30000);
    ~BufferManager();

    // Push new data into buffer
    void push(const std::vector<char>& data, long long timestamp = 0);

    // Get all current data in buffer (Thread-safe)
    std::deque<VideoPacket> getSnapshot();

    // Clear buffer
    void clear();

    // Set max duration
    void setMaxDuration(size_t ms);

    // Get current buffer usage
    size_t getSize() const;
    size_t getDuration() const;

private:
    std::deque<VideoPacket> buffer_;
    mutable std::mutex mutex_;
    size_t max_duration_ms_;
    size_t total_bytes_{0};
};

} // namespace recording
} // namespace vms
