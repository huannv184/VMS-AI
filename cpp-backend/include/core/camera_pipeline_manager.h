// ==============================================================
// File: include/core/camera_pipeline_manager.h
// COMPLETE VERSION with proper FFMPEG cleanup support
// ==============================================================

#pragma once

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <string>
#include <chrono>
#include <cstdio>
#include <optional>
#include <condition_variable>
#include <opencv2/core.hpp>

#include <nlohmann/json.hpp>
#include "core/camera_runtime_types.h"

// Forward declarations for batch inference
namespace inference { class BatchInferenceScheduler; class MultiModelInfer; struct TrackedObject; }

namespace vms {
namespace core {

// Forward Declaration of Context (Opaque Pointer)
struct PipelineContext;

/**
 * @brief Manager for camera video pipelines
 * 
 * Handles RTSP stream capture via FFMPEG and frame distribution
 * to WebSocket clients. Runs in monolith mode without external
 * AI server processes.
 */
class CameraPipelineManager {
public:
    static CameraPipelineManager& getInstance();

    CameraPipelineManager(); // Changed to user-declared to allow Pimpl
    ~CameraPipelineManager();
    
    // Non-copyable, non-movable
    CameraPipelineManager(const CameraPipelineManager&) = delete;
    CameraPipelineManager& operator=(const CameraPipelineManager&) = delete;
    CameraPipelineManager(CameraPipelineManager&&) = delete;
    CameraPipelineManager& operator=(CameraPipelineManager&&) = delete;
    
    /**
     * @brief Start video pipeline for a camera
     * 
     * @param camera_id Camera ID
     * @param rtsp_url RTSP stream URL
     * @return true if started successfully
     */
    bool startPipeline(int camera_id, const std::string& rtsp_url);
    
    /**
     * @brief Stop video pipeline for a camera
     * 
     * @param camera_id Camera ID
     */
    void stopPipeline(int camera_id);
    
    /**
     * @brief Stop all running pipelines
     */
    void stopAllPipelines();

    /**
     * @brief Start all configured pipelines (Auto-start)
     */
    void startAllPipelines();
    
    /**
     * @brief Check if pipeline is running
     * 
     * @param camera_id Camera ID
     * @return true if running
     */
    bool isRunning(int camera_id);

    /**
     * @brief Get latest frame for camera
     * 
     * @param camera_id Camera ID
     * @return Optional containing JPEG data if available
     */
    std::optional<std::vector<char>> getLatestFrame(int camera_id);

    /**
     * @brief Get latest tracked objects as JSON string
     *
     * @param camera_id Camera ID
     * @return JSON string of latest objects
     */
    std::string getLatestObjectsJson(int camera_id);

    /**
     * @brief Get latest metadata JSON
     */
    nlohmann::json getLatestMetadata(int camera_id);

    /**
     * @brief Get runtime statistics for camera
     */
    CameraStats getCameraStats(int camera_id);

    /**
     * @brief Get state-machine state for camera
     */
    CameraState getCameraState(int camera_id);

    /**
     * @brief Trigger event recording via BufferPipeline for a camera
     * 
     * Called by the Rule Engine when a RECORD_CLIP action fires.
     * 
     * @param camera_id Camera ID
     * @param event_id Unique event identifier
     * @param duration_sec Post-event recording duration (default: 20s)
     * @param pre_record_sec Pre-event buffer to include (default: 10s)
     * @return Video file path, or empty string on failure
     */
    std::string triggerEventRecording(int camera_id, const std::string& event_id,
                                      int duration_sec = 20, int pre_record_sec = 10);

private:
    /**
     * @brief Cleanup pipeline resources
     * 
     * @param camera_id Camera ID
     * 
     * NOTE: Caller must hold mutex_
     */
    void cleanupPipeline(int camera_id);

    // Thread for NativeLibavReader
    void workerNativeReader(int camera_id, PipelineContext* ctx);

    // Slots for async FFmpeg (QProcess) handling
    void handleH264Data(int camera_id, const QByteArray& data);
    void handleLogData(int camera_id, const QByteArray& data);
    void handleHlsLogData(int camera_id, const QByteArray& data);
    void handleAiLogData(int camera_id, const QByteArray& data);
    void handleFrameReady(int camera_id, const cv::Mat& raw_frame);

    // [OPTIMIZATION] Lean handler for pre-processed worker data
    void handleFrameProcessed(int camera_id, 
                             const cv::Mat& frame,
                             const std::vector<uchar>& jpeg_data,
                             const std::vector<inference::TrackedObject>& objects, 
                             const nlohmann::json& meta,
                             uint64_t timestamp);

    // Per-camera watchdog replaced by globalWatchdogTick().

    // Called via QueuedConnection when worker exhausts all reconnects
    void handleStreamFailed(int camera_id);

    // BUG-PM-RESTART-01 (2026-05-09): wired to FFmpegProcess::processStopped on
    // the per-camera ai_worker_v2 child. Pre-fix nothing listened to that
    // signal — when the AI worker exited (BUG-AIW-LOOP-01 50-failure bail,
    // segfault, OS kill, anything), the camera silently lost AI detection
    // until manual stop/start. Now: log + schedule respawn with backoff
    // (5s / 15s / 60s / 300s / give-up after 5 attempts in a sliding window).
    // Runs on the Qt main thread; the actual respawn happens via
    // QTimer::singleShot back onto qApp.
    void onAiWorkerStopped(int camera_id, int exit_code);
    void respawnAiWorkerOnQtThread(int camera_id);

    // Batch inference worker (replaces ai_worker.exe when batch inference enabled)
    void workerBatchInference(int camera_id, PipelineContext* ctx);

    // Lazy-init the shared batch scheduler from config
    void initBatchScheduler();

    // HealthMonitor owns the shared QTimer and invokes this callback every second.
    void globalWatchdogTick();

    // mutex_ guards pipelines_, failed_cameras_, using_backup_state_.
    // shared_lock  → concurrent HTTP reads (isRunning, getCameraStats, getLatestFrame …)
    // unique_lock  → startPipeline insert, stopPipeline erase, handleStreamFailed write
    // handleFrameProcessed uses shared_lock: it only reads the map pointer, modifies
    // per-context atomics (which are self-protected) and per-context sub-mutexes.
    std::shared_mutex mutex_;
    std::mutex restart_mutex_;  // BUG 2 FIX: Serializes stop→create sequence in startPipeline
    std::unordered_map<int, std::unique_ptr<PipelineContext>> pipelines_;  // Active pipelines
    std::unordered_map<int, bool> using_backup_state_; // Persist backup state across restarts
    // Restart backoff state (was duplicated here as dead code; HealthMonitor::setRestartBackoff
    // is the canonical sink for backoff counts and HealthMonitor owns the policy from now on).
    std::unordered_set<int> failed_cameras_; // Cameras that exhausted reconnects — no watchdog restart

    // Per-PID CPU sample cache: { pid → (cumulative cpu time in ns, last steady-clock sample) }.
    // Touched only from globalWatchdogTick (Qt main thread via HealthMonitor's QTimer)
    // — no mutex needed.
    struct CpuSample {
        uint64_t cpu_time_ns{0};
        std::chrono::steady_clock::time_point sampled_at{};
        bool initialized{false};
    };
    std::unordered_map<long long, CpuSample> cpu_sample_cache_;

    // Shared batch inference scheduler (one per GPU, outlives all pipelines)
    std::unique_ptr<inference::BatchInferenceScheduler> batch_scheduler_;
    bool batch_inference_enabled_ = false;
};

} // namespace core
} // namespace vms
