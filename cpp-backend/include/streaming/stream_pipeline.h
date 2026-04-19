// ==============================================================
// File: include/streaming/stream_pipeline.h
// Base Stream Pipeline Class
// ==============================================================

#pragma once

#include "streaming/stream_types.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>

namespace vms {
namespace core { 
    class FFmpegProcess;
}
namespace streaming {

// ============================================================================
// BASE STREAM PIPELINE
// ============================================================================

class StreamPipeline {
public:
    StreamPipeline(int camera_id, const StreamProfile& profile);
    virtual ~StreamPipeline();
    
    // Lifecycle (pure virtual - must implement)
    virtual bool start() = 0;
    virtual void stop();
    
    // Unblock pipes without cleaning up handles (called in Phase 1 shutdown)
    virtual void kill();
    
    // State queries
    bool isRunning() const;
    vms::core::PipelineState getState() const;
    StreamType getType() const { return profile_.type; }
    int getCameraID() const { return camera_id_; }
    
    // Metrics
    uint32_t getCurrentFPS() const { return current_fps_.load(); }
    uint64_t getBytesReceived() const { return bytes_received_.load(); }
    uint64_t getFramesProcessed() const { return frames_processed_.load(); }
    
    // Profile access
    const StreamProfile& getProfile() const { return profile_; }
    
protected:
    // Core state
    int camera_id_;
    StreamProfile profile_;
    std::atomic<vms::core::PipelineState> state_;
    std::atomic<bool> should_stop_{false};
    
    // FFmpeg process
    std::unique_ptr<core::FFmpegProcess> ffmpeg_;
    
    // Metrics
    std::atomic<uint32_t> current_fps_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> frames_processed_{0};
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_frame_time_;
    
    // Threading
    
    
    std::thread watchdog_thread_;
    std::mutex state_mutex_;
    
    std::atomic<bool> connection_lost_{false};
    
    // Helper methods
    virtual std::string buildFFmpegCommand() = 0;
    
    
    
    virtual void watchdogLoop();
    void updateFPS();
    
    // State transitions
    void setState(vms::core::PipelineState new_state);
};

} // namespace streaming
} // namespace vms
