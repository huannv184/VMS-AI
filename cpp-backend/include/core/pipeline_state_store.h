#pragma once

#include "core/camera_runtime_types.h"
#include "inference/tracking.h"

#include <cstdint>
#include <memory>
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

    // 2026-05-15 hot-path audit: stored as shared_ptr<const ...> so updateFrame
    // can build the buffer/objects OUTSIDE the store's unique_lock and only
    // swap pointers under the lock. Pre-fix updateFrame held unique_lock for
    // the 200-500 KB JPEG memcpy + objects copy at 30 fps × N cameras —
    // every other camera's reader (snapshot endpoint, stats poller) blocked
    // behind that memcpy on the single global mutex. After: lock hold is
    // pointer swap only; readers grab a refcounted handle in O(1).
    //
    // Empty/null pointer = "no frame yet". snapshot() returns these by-value
    // (= refcount bump, not deep copy) so the existing snapshot() callers
    // don't pay the deep-copy cost either.
    std::shared_ptr<const std::vector<char>> latest_frame_jpeg;
    std::shared_ptr<const std::vector<inference::TrackedObject>> latest_objects;

    // Metadata is updated by BOTH updateFrame AND the rare updateMetadata path
    // (ZmqEventBridge metadata messages). Kept as plain JSON because (a) its
    // copy cost is bounded — usually <2 KB per metadata blob — and (b) the
    // partial-update path doesn't have a frame bundle to swap with.
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
    // Zero-copy accessor for the JPEG bytes. Returns a refcounted handle to
    // the immutable buffer the writer published; callers can read its
    // contents without further locking and the underlying buffer survives
    // even if a newer frame replaces the published pointer. Prefer this for
    // hot read paths (snapshot endpoint, WS frame relay); the original
    // latestFrameJpeg() still does a deep copy on return for backward compat.
    std::shared_ptr<const std::vector<char>> latestFrameJpegShared(int camera_id) const;
    std::vector<inference::TrackedObject> latestObjects(int camera_id) const;
    nlohmann::json latestMetadata(int camera_id) const;
    std::optional<PipelineStateSnapshot> snapshot(int camera_id) const;

private:
    PipelineStateSnapshot& getOrCreateLocked(int camera_id);

    mutable std::shared_mutex mutex_;
    std::unordered_map<int, PipelineStateSnapshot> snapshots_;
};

} // namespace vms::core
