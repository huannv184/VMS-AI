#include "core/pipeline_state_store.h"

#include <mutex>

namespace vms::core {

PipelineStateStore& PipelineStateStore::getInstance() {
    static PipelineStateStore instance;
    return instance;
}

void PipelineStateStore::registerCamera(int camera_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.state = CameraState::CONNECTING;
    snapshot.is_running = false;
    snapshot.last_error.clear();
}

void PipelineStateStore::removeCamera(int camera_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    snapshots_.erase(camera_id);
}

void PipelineStateStore::updateFrame(int camera_id,
                                     const std::vector<uchar>& jpeg_data,
                                     const std::vector<inference::TrackedObject>& objects,
                                     const nlohmann::json& metadata,
                                     uint64_t timestamp_ms) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    if (!jpeg_data.empty()) {
        snapshot.latest_frame_jpeg.assign(jpeg_data.begin(), jpeg_data.end());
    }
    snapshot.latest_objects = objects;
    snapshot.latest_metadata = metadata;
    snapshot.last_frame_ts = static_cast<long long>(timestamp_ms);
}

void PipelineStateStore::updateMetadata(int camera_id, const nlohmann::json& metadata) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.latest_metadata = metadata;
}

void PipelineStateStore::updateStats(int camera_id,
                                     double fps,
                                     int restart_count,
                                     long long last_frame_ts,
                                     CameraState state,
                                     bool is_running) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.fps = fps;
    snapshot.restart_count = restart_count;
    snapshot.last_frame_ts = last_frame_ts;
    snapshot.state = state;
    snapshot.is_running = is_running;
    if (state != CameraState::FAILED) {
        snapshot.last_error.clear();
    }
}

void PipelineStateStore::updateCpuUsage(int camera_id, double percent) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.cpu_usage_percent = percent;
}

void PipelineStateStore::setState(int camera_id, CameraState state, const std::string& last_error) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.state = state;
    snapshot.is_running = (state != CameraState::FAILED);
    if (!last_error.empty()) {
        snapshot.last_error = last_error;
    } else if (state != CameraState::FAILED) {
        snapshot.last_error.clear();
    }
}

std::optional<std::vector<char>> PipelineStateStore::latestFrameJpeg(int camera_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = snapshots_.find(camera_id);
    if (it == snapshots_.end() || it->second.latest_frame_jpeg.empty()) {
        return std::nullopt;
    }
    return it->second.latest_frame_jpeg;
}

std::vector<inference::TrackedObject> PipelineStateStore::latestObjects(int camera_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = snapshots_.find(camera_id);
    if (it == snapshots_.end()) {
        return {};
    }
    return it->second.latest_objects;
}

nlohmann::json PipelineStateStore::latestMetadata(int camera_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = snapshots_.find(camera_id);
    if (it == snapshots_.end()) {
        return nlohmann::json::object();
    }
    return it->second.latest_metadata;
}

std::optional<PipelineStateSnapshot> PipelineStateStore::snapshot(int camera_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = snapshots_.find(camera_id);
    if (it == snapshots_.end()) {
        return std::nullopt;
    }
    return it->second;
}

PipelineStateSnapshot& PipelineStateStore::getOrCreateLocked(int camera_id) {
    return snapshots_[camera_id];
}

} // namespace vms::core
