#include "core/frame_bus_diagnostics.h"

#include "utils/logger.h"

namespace vms::core {

namespace {
constexpr auto kLogInterval = std::chrono::seconds(30);
constexpr const char* kConsumerId = "frame_bus_diagnostics";
} // namespace

FrameBusDiagnostics& FrameBusDiagnostics::getInstance() {
    static FrameBusDiagnostics instance;
    return instance;
}

std::shared_ptr<IFrameConsumer> FrameBusDiagnostics::sharedSelfRef() {
    // Static lifetime; null-deleter so shared_ptr never tries to destroy the singleton.
    // FrameBus stores weak_ptr — when this static is destroyed (process exit), any
    // surviving weak_ptr will simply lock() to null and the entry is skipped.
    static std::shared_ptr<IFrameConsumer> self_ref(
        &getInstance(), [](IFrameConsumer*) {});
    return self_ref;
}

void FrameBusDiagnostics::attach(int camera_id) {
    FrameBus::getInstance().subscribe(camera_id, kConsumerId, sharedSelfRef());
    {
        std::lock_guard<std::mutex> lock(per_camera_mutex_);
        per_camera_frames_.try_emplace(camera_id, 0);
    }
}

void FrameBusDiagnostics::detach(int camera_id) {
    FrameBus::getInstance().unsubscribe(camera_id, kConsumerId);
    {
        std::lock_guard<std::mutex> lock(per_camera_mutex_);
        per_camera_frames_.erase(camera_id);
    }
}

std::string FrameBusDiagnostics::consumerName() const {
    return kConsumerId;
}

void FrameBusDiagnostics::onFrame(const FrameEnvelope& frame) {
    // Hot path. Keep work O(1) and lockless on the fast branch.
    auto total = total_frames_.fetch_add(1, std::memory_order_relaxed) + 1;

    // Throttled per-camera bookkeeping + log. The log branch is rare (~once per 30s).
    auto now = std::chrono::steady_clock::now();
    bool should_log = false;
    uint64_t cam_count = 0;
    {
        std::lock_guard<std::mutex> lock(per_camera_mutex_);
        cam_count = ++per_camera_frames_[frame.camera_id];
        if (now - last_log_ >= kLogInterval) {
            last_log_ = now;
            should_log = true;
        }
    }

    if (should_log) {
        LOG_INFO("[FrameBusDiag] total={} camera={} cam_frames={} raw_frame={}",
                 total, frame.camera_id, cam_count,
                 frame.raw_frame ? "present" : "null");
    }
}

void FrameBusDiagnostics::onStreamStateChanged(int camera_id, CameraState state) {
    LOG_INFO("[FrameBusDiag] camera={} state={}", camera_id, static_cast<int>(state));
}

uint64_t FrameBusDiagnostics::framesFor(int camera_id) const {
    std::lock_guard<std::mutex> lock(per_camera_mutex_);
    auto it = per_camera_frames_.find(camera_id);
    return it == per_camera_frames_.end() ? 0 : it->second;
}

} // namespace vms::core
