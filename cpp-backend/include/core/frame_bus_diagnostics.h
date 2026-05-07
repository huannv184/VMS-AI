#pragma once

#include "core/frame_bus.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vms::core {

// Lightweight subscriber that proves the FrameBus contract end-to-end and exposes
// per-camera frame-count + state-change metrics. Designed to be hot-path safe:
// onFrame() is a single relaxed atomic increment plus a throttled log.
class FrameBusDiagnostics : public IFrameConsumer {
public:
    static FrameBusDiagnostics& getInstance();

    // Subscribe / unsubscribe this singleton from a camera's FrameBus stream.
    // Safe to call repeatedly; idempotent.
    void attach(int camera_id);
    void detach(int camera_id);

    // IFrameConsumer
    std::string consumerName() const override;
    void onFrame(const FrameEnvelope& frame) override;
    void onStreamStateChanged(int camera_id, CameraState state) override;

    // Read-only metrics (snapshot, lock-free for total).
    uint64_t totalFrames() const { return total_frames_.load(std::memory_order_relaxed); }
    uint64_t framesFor(int camera_id) const;

private:
    FrameBusDiagnostics() = default;

    static std::shared_ptr<IFrameConsumer> sharedSelfRef();

    std::atomic<uint64_t> total_frames_{0};

    mutable std::mutex per_camera_mutex_;
    std::unordered_map<int, uint64_t> per_camera_frames_;
    std::chrono::steady_clock::time_point last_log_{};
};

} // namespace vms::core
