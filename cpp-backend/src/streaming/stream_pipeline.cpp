// ==============================================================
// File: src/streaming/stream_pipeline.cpp
// Base Stream Pipeline Implementation
// Thread-safety: state_ is std::atomic. Safe for concurrent reads.
// ==============================================================

#include "streaming/stream_pipeline.h"
#include "streaming/stream_types.h"  // For PipelineState
#include "core/ffmpeg_process.h"
#include "utils/logger.h"
#include <sstream>

namespace vms::streaming {

// ============================================================================
// STREAM PIPELINE BASE CLASS
// ============================================================================

StreamPipeline::StreamPipeline(int camera_id, const StreamProfile& profile)
    : camera_id_(camera_id)
    , profile_(profile)
    , state_(vms::core::PipelineState::STOPPED)
    , should_stop_(false) // Added
    , frames_processed_(0) // Added
    , bytes_received_(0) // Added
    , current_fps_(0.0f) // Added
{
    // start_time_ = std::chrono::steady_clock::now(); // Removed as per example
    LOG_INFO("[StreamPipeline-{}-{}] Created", camera_id_, static_cast<int>(profile_.type));
}

StreamPipeline::~StreamPipeline() {
    LOG_INFO("[StreamPipeline-{}-{}] Destructor", camera_id_, static_cast<int>(profile_.type));
    // should_stop_ = true; // Replaced by stop()
    
    // Stop FFmpeg // Replaced by stop()
    // if (ffmpeg_) {
    //     ffmpeg_->stop();
    // }
    
    // Join threads // Replaced by stop()
    // if (worker_thread_.joinable()) worker_thread_.join();
    // if (stderr_thread_.joinable()) stderr_thread_.join();
    stop(); // Added as per example
}

bool StreamPipeline::isRunning() const {
    auto s = state_.load(); // Changed from atomic to regular, needs mutex
    return s == vms::core::PipelineState::RUNNING || s == vms::core::PipelineState::DEGRADED; // Modified
}

vms::core::PipelineState StreamPipeline::getState() const {
    return state_.load(); // Changed from atomic to regular, needs mutex
}

void StreamPipeline::setState(vms::core::PipelineState new_state) {
    auto old_state = state_.exchange(new_state); // Changed from atomic to regular, needs mutex
    if (old_state != new_state) {
        // LOG_INFO("[StreamPipeline-{}-{}] State: {} → {}", 
        //         camera_id_, 
        //         static_cast<int>(profile_.type),
        //         static_cast<int>(old_state),
        //         static_cast<int>(new_state));
        LOG_INFO("[StreamPipeline-{}-{}] State: {} -> {}", 
                camera_id_, static_cast<int>(profile_.type),
                static_cast<int>(old_state), static_cast<int>(new_state));
    }
}


void StreamPipeline::updateFPS() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
    
    if (elapsed.count() > 0) {
        current_fps_ = static_cast<uint32_t>(frames_processed_.load() / elapsed.count());
    }
}

void StreamPipeline::stop() {
    // Base stop implementation
    // derived classes should call this or handle their own stop
    should_stop_ = true;
}

void StreamPipeline::kill() {
    should_stop_ = true;
    if (ffmpeg_) ffmpeg_->kill();
}

void StreamPipeline::watchdogLoop() {
    // Default empty watchdog - derived classes override as needed
}

} // namespace vms::streaming
