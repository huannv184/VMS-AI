#include "recording/buffer_manager.h"
#include "utils/logger.h"

namespace vms {
namespace recording {

BufferManager::BufferManager(size_t max_duration_ms) 
    : max_duration_ms_(max_duration_ms) {
}

BufferManager::~BufferManager() {
    clear();
}

void BufferManager::push(const std::vector<char>& data, long long timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    VideoPacket packet;
    packet.data = data;
    packet.timestamp = timestamp;
    
    buffer_.push_back(packet);
    total_bytes_ += data.size();

    if (buffer_.empty()) return;

    long long newest = buffer_.back().timestamp;
    
    // Prune by time
    while (!buffer_.empty()) {
        long long oldest = buffer_.front().timestamp;
        if (newest - oldest > static_cast<long long>(max_duration_ms_)) {
            total_bytes_ -= buffer_.front().data.size();
            buffer_.pop_front();
        } else {
            break;
        }
    }
    
    // Safety cap: Max 50MB (52428800 bytes) to prevent OOM.
    // If a single packet exceeds the cap we end up popping it back out, which
    // is silent data loss — log so operators notice mis-configured cameras
    // pushing huge keyframes (4K HEVC GOP can spike >> typical packet size).
    constexpr size_t kSafetyCapBytes = 52428800;
    bool dropped_under_cap = false;
    while (!buffer_.empty() && total_bytes_ > kSafetyCapBytes) {
        total_bytes_ -= buffer_.front().data.size();
        buffer_.pop_front();
        dropped_under_cap = true;
    }
    if (dropped_under_cap) {
        LOG_THROTTLED_WARN(10000,
            "[BufferManager] 50MB safety cap exceeded — dropped older packets. "
            "Single packet size={} bytes (likely keyframe).", data.size());
    }
}

std::deque<VideoPacket> BufferManager::getSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_; // Returns a copy
}

void BufferManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    total_bytes_ = 0;
}

void BufferManager::setMaxDuration(size_t ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_duration_ms_ = ms;
}

size_t BufferManager::getSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.size();
}

size_t BufferManager::getDuration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_.empty()) return 0;
    return buffer_.back().timestamp - buffer_.front().timestamp;
}

} // namespace recording
} // namespace vms
