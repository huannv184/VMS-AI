#pragma once

#include <cstdint>

namespace vms::core {

enum class CameraState : int {
    CONNECTING = 0,
    RUNNING    = 1,
    DEGRADED   = 2,
    FAILED     = 3
};

inline const char* cameraStateToString(CameraState s) noexcept {
    switch (s) {
        case CameraState::CONNECTING: return "CONNECTING";
        case CameraState::RUNNING:    return "RUNNING";
        case CameraState::DEGRADED:   return "DEGRADED";
        case CameraState::FAILED:     return "FAILED";
    }
    return "UNKNOWN";
}

struct CameraStats {
    bool is_running = false;
    double fps = 0.0;
    int restart_count = 0;
    long long last_frame_ts = 0;
    CameraState state = CameraState::CONNECTING;
    // Aggregated CPU% across this pipeline's child processes (FFmpeg + ai_worker).
    // -1 = "not yet sampled"; controller treats -1 as "unknown" not "0".
    double cpu_usage_percent = -1.0;
};

} // namespace vms::core
