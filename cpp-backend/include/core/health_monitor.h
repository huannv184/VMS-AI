#pragma once

#include "core/camera_runtime_types.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class QTimer;

namespace vms::core {

struct CameraHealthSnapshot {
    bool registered{false};
    bool permanent_failure{false};
    CameraState state{CameraState::CONNECTING};
    uint64_t last_frame_ts{0};
    int64_t last_state_change_ms{0};
    std::string last_error;
};

class HealthMonitor {
public:
    static HealthMonitor& getInstance();

    void start(std::function<void()> tick_callback, int interval_ms = 1000);
    void stop();

    void registerCamera(int camera_id);
    void unregisterCamera(int camera_id);

    void updateFrameHeartbeat(int camera_id, uint64_t timestamp_ms);
    void setState(int camera_id, CameraState state, const std::string& last_error = {});
    void clearFailure(int camera_id);
    void markPermanentFailure(int camera_id, const std::string& reason);

    // Restart backoff lives in NativeReaderWorker (where the reconnect loop runs).
    // HealthMonitor used to expose a setRestartBackoff() sink — removed Phase B
    // 2026-05-06 because no caller wrote it and no consumer read it.

    std::optional<CameraHealthSnapshot> snapshot(int camera_id) const;

private:
    static int64_t steadyNowMs();
    CameraHealthSnapshot& getOrCreateLocked(int camera_id);

    mutable std::shared_mutex mutex_;            // Guards `snapshots_` only.
    std::unordered_map<int, CameraHealthSnapshot> snapshots_;

    // Lifecycle mutex protects `timer_` and `tick_callback_` from concurrent
    // start()/stop() and from races between stop() and the QTimer-fired tick lambda.
    mutable std::mutex lifecycle_mutex_;
    std::function<void()> tick_callback_;
    QTimer* timer_{nullptr};
};

} // namespace vms::core
