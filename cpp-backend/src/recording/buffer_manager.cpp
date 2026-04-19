#include "recording/buffer_manager.h"

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
    // packet.is_keyframe = ... (Need deeper parsing for this, optional for now if using TS)
    
    buffer_.push_back(packet);

    // Prune old packets
    if (buffer_.empty()) return;

    // Simple pruning based on count or timestamp?
    // Timestamp is better but requires reliable input.
    // If timestamp is 0, we might strictly rely on size limit or estimate.
    // Let's assume timestamp is valid system time for now.
    
    long long newest = buffer_.back().timestamp;
    
    while (!buffer_.empty()) {
        long long oldest = buffer_.front().timestamp;
        if (newest - oldest > static_cast<long long>(max_duration_ms_)) {
            buffer_.pop_front();
        } else {
            break;
        }
    }
    
    // Safety cap: Max 50MB to prevent OOM if timestamps are broken
    size_t total_size = 0; // This is slow to calc every time. 
    // Optimization: we could track total_size in a member variable.
    // implementing checking logic only if buffer is very large
    if (buffer_.size() > 5000) { // Approx 5000 chunks
        buffer_.pop_front();
    }
}

std::deque<VideoPacket> BufferManager::getSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_; // Returns a copy
}

void BufferManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
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
