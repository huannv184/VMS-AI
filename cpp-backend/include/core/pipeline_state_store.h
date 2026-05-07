#pragma once

#include "core/camera_runtime_types.h"
#include "inference/tracking.h"

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace vms::core {

struct PipelineStateSnapshot {
    bool is_running{false};
    double fps{0.0};
    int restart_count{0};
    long long last_frame_ts{0};
    CameraState state{CameraState::CONNECTING};
    // Aggregated CPU% across this pipeline's child processes (FFmpeg + ai_worker).
    // Populated by globalWatchdogTick at 1 Hz; -1 means "not yet sampled".
    double cpu_usage_percent{-1.0};
    std::string last_error;
    std::vector<char> latest_frame_jpeg;
    std::vector<inference::TrackedObject> latest_objects;
    nlohmann::json latest_metadata{nlohmann::json::object()};
};

class PipelineStateStore {
public:
    static PipelineStateStore& getInstance();

    void registerCamera(int camera_id);
    void removeCamera(int camera_id);

    void updateFrame(int camera_id,
                     const std::vector<uchar>& jpeg_data,
                     const std::vector<inference::TrackedObject>& objects,
                     const nlohmann::json& metadata,
                     uint64_t timestamp_ms);
    void updateMetadata(int camera_id, const nlohmann::json& metadata);
    void updateStats(int camera_id,
                     double fps,
                     int restart_count,
                     long long last_frame_ts,
                     CameraState state,
                     bool is_running);
    void setState(int camera_id, CameraState state, const std::string& last_error = {});
    void updateCpuUsage(int camera_id, double percent);

    std::optional<std::vector<char>> latestFrameJpeg(int camera_id) const;
    std::vector<inference::TrackedObject> latestObjects(int camera_id) const;
    nlohmann::json latestMetadata(int camera_id) const;
    std::optional<PipelineStateSnapshot> snapshot(int camera_id) const;

private:
    PipelineStateSnapshot& getOrCreateLocked(int camera_id);

    mutable std::shared_mutex mutex_;
    std::unordered_map<int, PipelineStateSnapshot> snapshots_;
};

} // namespace vms::core
