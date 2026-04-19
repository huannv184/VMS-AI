#ifndef NOMINMAX
#define NOMINMAX
#endif
// ==============================================================
// File: src/core/camera_pipeline_manager.cpp
// COMPLETE FIXED VERSION with proper FFMPEG cleanup
// ==============================================================

#include <QThread>
#include <QTimer>
#include <QMetaObject>
#include <QObject>
#include <QCoreApplication>
#include "core/native_reader_worker.h"
#include <QByteArray>
#include "streaming/camera_stream_manager_qt.h"
#include "streaming/mediamtx_publisher.h"
#include "core/camera_pipeline_manager.h"
#include "core/ffmpeg_process.h"
#include "recording/buffer_pipeline.h"
#include "recording/continuous_recorder.h"
#include "core/camera_manager.h"
#include "database/models.h"
#include "utils/logger.h"
#include "utils/config.h" // Added for config access
#include "database/camera_repository.h" // Added for camera name lookup
#include <cstdint> // Added for uint64_t
#include <filesystem>
#include <nlohmann/json.hpp>
#include "utils/api_utils.h"
#include <opencv2/opencv.hpp>
#include <sstream>
#include <fstream>
#include <chrono>
#include <filesystem>
#include <set>
#include "core/event_manager.h"
#include <algorithm>
#include <vector>
#include <cstring>
#include <atomic>
#include <cmath>  // For std::pow in exponential backoff
#include <thread>
#include <mutex>
#include "inference/tracking.h"
#include "inference/batch_inference_scheduler.h"
#include "inference/multi_model_infer.h" // Needed for TrackedObject in PipelineContext
#include "ai/ipc/shared_memory_manager.h" // Added for SHM communication
#include "ipc/zmq_event_bridge.h"
#include "core/ai_event_processor.h"
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

using json = nlohmann::json;

namespace vms {
namespace core {

// File-level constants to avoid magic numbers
namespace {
    constexpr int STREAMING_FPS = 15;
    constexpr size_t FRAME_WIDTH = 640;
    constexpr size_t FRAME_HEIGHT = 360;
    constexpr size_t FRAME_SIZE = FRAME_WIDTH * FRAME_HEIGHT * 3;
    constexpr int WATCHDOG_CHECK_INTERVAL_SEC = 5;
    constexpr int FRAME_TIMEOUT_MS = 15000;
    constexpr size_t MAX_FRAME_BUFFER_SIZE = 10 * 1024 * 1024; // 10MB safety limit
}

// Definition of PipelineContext (Moved from header to hide implementation details)
struct PipelineContext {
    std::unique_ptr<NativeStreamReader> native_reader;
    std::unique_ptr<QThread> native_reader_thread;
    // Non-owning pointer to the worker for fast fps updates from globalWatchdogTick().
    // Valid as long as native_reader_thread is alive (i.e., while this ctx is in pipelines_).
    NativeReaderWorker* worker_ptr{nullptr};

    std::unique_ptr<FFmpegProcess> process; // Used as h264_process

    // State
    std::atomic<bool> running{false};
    std::atomic<bool> should_stop{false};
    
    // AI Integration
    std::unique_ptr<FFmpegProcess> ai_process;
    
    // MediaMTX WebRTC Publisher (forwards raw H264 → MediaMTX RTSP → WebRTC WHEP)
    std::unique_ptr<vms::streaming::MediaMtxPublisher> mediamtx_publisher;
    
    // Shared Memory Manager
    std::unique_ptr<::ipc::SharedMemoryManager> shm_manager;
    
    // Buffer Pipeline (RAM Circular Buffer)
    std::unique_ptr<vms::recording::BufferPipeline> buffer_pipeline;
    
    // Continuous 24/7 Recorder (segment-based)
    std::unique_ptr<vms::recording::ContinuousRecorder> continuous_recorder;
    std::mutex objects_mutex;
    std::vector<inference::TrackedObject> latest_objects;
    
    std::mutex frame_mutex;
    std::vector<char> latest_frame;
    
    std::atomic<int64_t> last_frame_ts{0};
    std::atomic<double> current_fps{0.0};
    std::atomic<int> restart_count{0};
    std::atomic<int> no_frame_count{0};
    std::atomic<int> camera_state{static_cast<int>(CameraState::CONNECTING)};
    
    std::chrono::steady_clock::time_point last_log_time;
    std::chrono::steady_clock::time_point last_ai_log_time;          // PERF: throttle AI detection logging
    std::chrono::steady_clock::time_point last_event_process_time;   // PERF: throttle AiEventProcessor
    uint64_t frame_count_{0};                                         // PERF: per-camera frame counter

    std::string rtsp_url;
    std::string sub_stream_url;
    std::string camera_name;
    bool using_backup_stream = false;
    std::chrono::steady_clock::time_point start_time;
    nlohmann::json last_metadata_json_;

    // Explicit destructor: enforce safe shutdown order (no per-camera QTimer anymore;
    // global watchdog in CameraPipelineManager handles all cameras).
    ~PipelineContext() {
        should_stop = true;
        running = false;

        // 1. Join the NativeReaderWorker thread FIRST.
        //    The worker emits signals into the Qt event loop; joining it guarantees
        //    no further signals will be emitted after this point.
        if (native_reader_thread) {
            auto* worker = dynamic_cast<NativeReaderWorker*>(native_reader_thread.get());
            if (worker) {
                worker->stop();
            } else if (native_reader_thread->isRunning()) {
                native_reader_thread->quit();
                native_reader_thread->wait(5000);
            }
        }

        // 3. Stop recording subsystems.
        if (buffer_pipeline)     buffer_pipeline->stop();
        if (continuous_recorder) continuous_recorder->stop();

        // 4. Stop media relay and AI subprocess.
        if (mediamtx_publisher) mediamtx_publisher->stop();
        if (ai_process)         ai_process->stop();
        if (process)            process->stop();

        // Remaining members (shm_manager, native_reader, frames, etc.)
        // are cleaned up by their own destructors in reverse declaration order.
    }
};




// ============================================================================
// MANAGER IMPLEMENTATION
// ============================================================================

CameraPipelineManager::CameraPipelineManager() {
    // NOTE: We intentionally do NOT kill processes by name on startup.
    // Doing so can terminate unrelated processes on the host.
    // If "Address in use" happens after a crash, we should handle it by
    // retrying binds / using per-instance ports and by shutting down child
    // processes cleanly during normal stop().
}

void CameraPipelineManager::startAllPipelines() {
    LOG_INFO("Auto-starting all camera pipelines...");
    auto& camera_manager = vms::core::CameraManager::getInstance();
    auto cameras = camera_manager.getAllCameras();
    
    for (const auto& cam : cameras) {
        if (cam.is_active || !cam.rtsp_url.empty()) {
            LOG_INFO("Auto-starting camera {}: {}", cam.id, cam.name);
            startPipeline(cam.id, cam.rtsp_url);
        }
    }
}

CameraPipelineManager::~CameraPipelineManager() {
    // Stop global watchdog before destroying pipelines so the tick never fires
    // on a partially-destroyed pipeline map.
    if (global_watchdog_timer_) {
        QTimer* t = global_watchdog_timer_;
        global_watchdog_timer_ = nullptr;
        if (QThread::currentThread() == t->thread()) {
            t->stop();
            delete t;
        } else {
            QMetaObject::invokeMethod(t, [t]() { t->stop(); t->deleteLater(); },
                                      Qt::BlockingQueuedConnection);
        }
    }
    stopAllPipelines();
}

CameraPipelineManager& CameraPipelineManager::getInstance() {
    static CameraPipelineManager instance;
    return instance;
}

bool CameraPipelineManager::startPipeline(int camera_id, const std::string& rtsp_url) {
    // BUG 2 FIX: Serialize the entire stop→create sequence.
    // Without this, 6 concurrent restarts from Health Supervisor can race:
    // old pipeline destructor runs while new one is being created → use-after-free.
    std::lock_guard<std::mutex> restart_lock(restart_mutex_);

    {
        std::unique_ptr<PipelineContext> old_ctx;
        {
            std::lock_guard<std::shared_mutex> lock(mutex_);
            auto it = pipelines_.find(camera_id);
            if (it != pipelines_.end()) {
                LOG_INFO("Stopping existing pipeline for camera {} before restart", camera_id);
                old_ctx = std::move(it->second);
                pipelines_.erase(it);
            }
        }
        // BUG 2 FIX: Force complete destruction of old pipeline BEFORE creating new one.
        // This ensures NativeReaderWorker thread is joined and all resources released.
        if (old_ctx) {
            LOG_INFO("Waiting for old pipeline destruction for camera {}...", camera_id);
            old_ctx.reset();  // Explicit destruction — blocks until NativeReaderWorker::stop() returns
            LOG_INFO("Old pipeline destroyed for camera {}", camera_id);
        }
    }

    // Clear FAILED state on (re)start — operator may have fixed the camera
    {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        if (failed_cameras_.erase(camera_id)) {
            LOG_INFO("[STATE] Camera {} cleared from FAILED set — restarting", camera_id);
        }
    }

    LOG_INFO("Starting pipeline for camera {} (URL: {})", camera_id, rtsp_url);
    
    auto& camera_mgr = vms::core::CameraManager::getInstance();
    std::optional<Camera> camera_opt = std::nullopt;
    
    try { camera_opt = camera_mgr.getCamera(camera_id); } 
    catch (...) { return false; }

    if (!camera_opt) return false;
    std::string sub_url = camera_opt->sub_stream_url;
    std::string camera_name_cached = camera_opt->name.empty() ? "Camera " + std::to_string(camera_id) : camera_opt->name;

    auto ctx = std::make_unique<PipelineContext>();
    ctx->camera_name = camera_name_cached;
    ctx->rtsp_url = rtsp_url;
    ctx->sub_stream_url = sub_url;
    
    // BUG 3 FIX: Initialize last_frame_ts to current system time to avoid immediate watchdog timeout.
    auto now_init = std::chrono::system_clock::now();
    ctx->last_frame_ts = std::chrono::duration_cast<std::chrono::milliseconds>(now_init.time_since_epoch()).count();
    // Record pipeline start time for watchdog grace period
    ctx->start_time = std::chrono::steady_clock::now();
    
    // Detect model path relative to executable or CWD
    std::string model_path = "models/yolo11m.engine";
    if (!std::filesystem::exists(model_path)) {
        // Try relative to the executable directory (e.g., build/Release/)
        auto exe_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
        auto alt_path = exe_dir / "models" / "yolo11m.engine";
        if (std::filesystem::exists(alt_path)) {
            model_path = alt_path.string();
            LOG_INFO("[Manager] Using model from exe dir: {}", model_path);
        }
    }
    if (std::filesystem::exists(model_path)) {
        ctx->ai_process = std::make_unique<FFmpegProcess>();
        // Move FFmpegProcess to Qt main thread.  Also explicitly move its
        // QProcess VALUE member, which moveToThread() skips because value
        // members are not in the Qt parent-child tree.
        // Both calls must come from the current thread (the owner of process_).
        ctx->ai_process->moveToThread(QCoreApplication::instance()->thread());
        ctx->ai_process->moveProcessToThread(QCoreApplication::instance()->thread());
        std::string db_path = vms::Config::getInstance().getDatabaseConfig().path;
        std::string ai_config_json = camera_opt->ai_config.empty() ? "{}" : camera_opt->ai_config;
        std::string escaped_json;
        for (char c : ai_config_json) escaped_json += (c == '"' ? "\"" : std::string(1, c));

        // Detect AI worker executable path robustly
        std::string ai_worker_exe = "ai_worker_v2.exe";
        if (!std::filesystem::exists(ai_worker_exe)) {
            auto exe_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
            // Try in the same folder as backend
            if (std::filesystem::exists(exe_dir / "ai_worker_v2.exe")) {
                ai_worker_exe = (exe_dir / "ai_worker_v2.exe").string();
            } else if (std::filesystem::exists(exe_dir.parent_path() / "ai_worker_v2.exe")) {
                // Try in parent folder (build/ when running from build/Release/)
                ai_worker_exe = (exe_dir.parent_path() / "ai_worker_v2.exe").string();
            }
        }
        // Convert to absolute paths — AI worker CWD may differ
        std::string abs_model = std::filesystem::absolute(model_path).string();
        std::string abs_db = std::filesystem::absolute(db_path).string();

        std::string ai_cmd = "\"" + ai_worker_exe + "\" " + std::to_string(camera_id) + " \"" + abs_model + "\" \"" + abs_db + "\" \"" + escaped_json + "\"";
        LOG_INFO("[Manager] Launching AI Worker for camera {}: {}", camera_id, ai_cmd);
        
        QObject::connect(ctx->ai_process.get(), &vms::core::FFmpegProcess::stdoutReady, ctx->ai_process.get(),
                         [this, camera_id](const QByteArray& data) { handleAiLogData(camera_id, data); });
        QObject::connect(ctx->ai_process.get(), &vms::core::FFmpegProcess::stderrReady, ctx->ai_process.get(),
                         [this, camera_id](const QByteArray& data) { handleAiLogData(camera_id, data); });
        
        // start() must run on the object's owner thread (main Qt thread).
        // startPipeline() may be called from a Crow HTTP thread — marshal with blocking call.
        bool ai_started = false;
        auto* ai_proc = ctx->ai_process.get();
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            ai_started = ai_proc->start(ai_cmd);
        } else {
            QMetaObject::invokeMethod(ai_proc, [ai_proc, &ai_cmd, &ai_started]() {
                ai_started = ai_proc->start(ai_cmd);
            }, Qt::BlockingQueuedConnection);
        }
        if (!ai_started) {
            LOG_ERROR("[Manager] ❌ Failed to start AI Worker for camera {}", camera_id);
            ctx->ai_process.reset();
        } else {
            LOG_INFO("[Manager] ✅ AI Worker started for camera {}", camera_id);
        }
    } else {
        LOG_WARN("[Manager] ⚠ Model not found at '{}' — AI disabled for camera {}", model_path, camera_id);
    }
    
    ctx->shm_manager = std::make_unique<::ipc::SharedMemoryManager>(camera_id);
    ctx->shm_manager->initialize();
    
    vms::streaming::StreamProfile rec_profile; rec_profile.rtsp_url = rtsp_url;
    ctx->buffer_pipeline = std::make_unique<vms::recording::BufferPipeline>(camera_id, rec_profile);
    ctx->buffer_pipeline->start();
    
    // The stream will be opened by NativeReaderWorker::run() in its own thread.
    
    auto* worker = new NativeReaderWorker(camera_id, rtsp_url);
    ctx->native_reader_thread.reset(worker);
    ctx->worker_ptr = worker; // non-owning; valid for lifetime of ctx
    
    // BUG 2 FIX: Ensure signals are delivered to the main thread's event loop.
    // Use QCoreApplication::instance() as context for the lambda connection.
    QObject::connect(worker, &vms::core::NativeReaderWorker::frameReady, qApp,
                     [this, camera_id](const cv::Mat& frame) { handleFrameReady(camera_id, frame); }, Qt::QueuedConnection);
    QObject::connect(worker, &vms::core::NativeReaderWorker::rawPacketReady, qApp,
                     [this, camera_id](const QByteArray& data, bool isKeyframe) {
                         std::shared_lock<std::shared_mutex> lock(mutex_);
                         auto it = pipelines_.find(camera_id);
                         if (it != pipelines_.end()) {
                             auto raw_ctx = it->second.get();
                             if (raw_ctx->buffer_pipeline) {
                                 raw_ctx->buffer_pipeline->writeRawData(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
                             }
                         }
                     }, Qt::QueuedConnection);
                     
    // FaceID Phase 2: Metadata update from ZmqEventBridge
    QObject::connect(&vms::ipc::ZmqEventBridge::getInstance(), &vms::ipc::ZmqEventBridge::eventReceived, qApp,
                     [this, camera_id](const QString& type, const QString& raw_json) {
                         if (type == "metadata" || type == "METADATA") {
                             std::shared_lock<std::shared_mutex> lock(mutex_);
                             auto it = pipelines_.find(camera_id);
                             if (it != pipelines_.end()) {
                                 try {
                                     auto j = nlohmann::json::parse(raw_json.toStdString());
                                     if (j.value("camera_id", -1) == camera_id) {
                                         it->second->last_metadata_json_ = j;
                                     }
                                 } catch (...) {}
                             }
                         }
                     });
                     
    // [R10] MediaMTX WebRTC Publisher ─────────────────────────────────────
    // Forward raw H264 packets to MediaMTX for WebRTC WHEP delivery.
    // This runs a separate FFmpeg process: pipe:0 -> rtsp://localhost:8554/cam_<id>
    {
        // [R6] Prepare config for publisher. 
        // Note: In a full implementation, these would be loaded from the database into the Camera model.
        // For now, we use defaults as per R5/R6 requirements.
        CameraConfig cfg;
        cfg.mediamtx_url = ""; // Will fallback to rtsp://localhost:8554 in publisher
        cfg.ffmpeg_path = "";  // Will fallback to "ffmpeg" in publisher

        // [R10] Fetch stream profile for the SUB stream
        vms::streaming::StreamProfile profile = vms::streaming::StreamProfile::createSub(rtsp_url);

        ctx->mediamtx_publisher = std::make_unique<vms::streaming::MediaMtxPublisher>(camera_id, profile, cfg);
        // Move to Qt main thread so QTimer children and QProcess (created in start()) live there.
        ctx->mediamtx_publisher->moveToThread(QCoreApplication::instance()->thread());

        // start() creates QProcess and starts QTimers — must run on owner thread.
        bool pub_started = false;
        auto* pub = ctx->mediamtx_publisher.get();
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            pub_started = pub->start();
        } else {
            QMetaObject::invokeMethod(pub, [pub, &pub_started]() {
                pub_started = pub->start();
            }, Qt::BlockingQueuedConnection);
        }
        if (pub_started) {
            LOG_INFO("[Manager] MediaMTX publisher started for camera {} -> {}",
                     camera_id, ctx->mediamtx_publisher->getPublishUrl().toStdString());
        } else {
            LOG_WARN("[Manager] MediaMTX publisher failed for camera {} (WebRTC fallback to WS only)", camera_id);
        }
    }

    // [R10] Connect rawPacketReady -> MediaMTX publisher
    if (ctx->mediamtx_publisher) {
        // [R2] Qt::QueuedConnection is mandatory for thread-safe cross-thread signaling
        QObject::connect(worker, &vms::core::NativeReaderWorker::rawPacketReady,
                         ctx->mediamtx_publisher.get(), &vms::streaming::MediaMtxPublisher::onPacketReady,
                         Qt::QueuedConnection);
    }

    // [OPTIMIZATION] Set SHM manager and connect processed signal
    worker->setShmManager(ctx->shm_manager.get());
    QObject::connect(worker, &vms::core::NativeReaderWorker::frameProcessed, qApp,
                     [this](int camera_id, const std::vector<uchar>& jpeg_data,
                            const std::vector<inference::TrackedObject>& objects,
                            const nlohmann::json& meta, uint64_t timestamp) {
                         handleFrameProcessed(camera_id, jpeg_data, objects, meta, timestamp);
                     }, Qt::QueuedConnection);

    // Permanent failure signal — stops watchdog from restarting a dead camera
    QObject::connect(worker, &vms::core::NativeReaderWorker::streamFailed, qApp,
                     [this, camera_id]() { handleStreamFailed(camera_id); },
                     Qt::QueuedConnection);

    worker->start();
    
    // [OPTIMIZATION] Staggered startup to avoid connection flood to Dahua/Hikvision cameras
    QThread::msleep(500); 

    // Start Continuous Recorder for 24/7 segment-based
    // Uses sub_stream_url to avoid Dahua TCP drop on main stream due to multiple connections
    std::string rec_url = sub_url.empty() ? rtsp_url : sub_url;
    ctx->continuous_recorder = std::make_unique<vms::recording::ContinuousRecorder>(
        camera_id, rec_url, "recordings", 300, 7); // 5-min segments, 7-day retention
    ctx->continuous_recorder->start();
    LOG_INFO("[Manager] ContinuousRecorder started for camera {} on url {}", camera_id, rec_url);
    
    // BUG 3 FIX: Mark pipeline as running immediately to prevent supervisor restarts.
    ctx->running = true;
    LOG_INFO("[Manager] Pipeline {} marked as running with last_frame_ts = {}", camera_id, ctx->last_frame_ts.load());
    
    // Lazy-init the single global watchdog timer (protected by restart_mutex_).
    // All cameras share one QTimer that ticks every 1 s — replaces N per-camera timers.
    if (!global_watchdog_timer_) {
        global_watchdog_timer_ = new QTimer();
        global_watchdog_timer_->moveToThread(QCoreApplication::instance()->thread());
        QObject::connect(global_watchdog_timer_, &QTimer::timeout,
                         [this]() { globalWatchdogTick(); });
        QTimer* gw = global_watchdog_timer_;
        QMetaObject::invokeMethod(gw, [gw]() { gw->start(1000); }, Qt::QueuedConnection);
        LOG_INFO("[Manager] Global watchdog timer started (1 s interval)");
    }

    std::lock_guard<std::shared_mutex> lock(mutex_);
    pipelines_[camera_id] = std::move(ctx);
    LOG_INFO("Pipeline started successfully for camera {}", camera_id);
    return true;
}

// REMOVED legacy runCaptureLoop

// CLEANUP & STOP LOGIC (Refactored for Multi-thread Process)
// ============================================================================

void CameraPipelineManager::cleanupPipeline(int camera_id) {
    LOG_INFO("Delegating cleanup to stopPipeline for camera {}", camera_id);
    stopPipeline(camera_id);
}


void CameraPipelineManager::stopPipeline(int camera_id) {
    std::unique_ptr<PipelineContext> ctx_to_stop;
    {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        auto it = pipelines_.find(camera_id);
        if (it == pipelines_.end()) return;
        
        // Move ownership out of map FIRST to prevent any new signal handlers
        // from finding this pipeline while we are shutting it down.
        ctx_to_stop = std::move(it->second);
        pipelines_.erase(it);
    }
    // Mutex unlocked — prevents DEADLOCK with watchdog/frame handlers.
    // The explicit ~PipelineContext() destructor handles safe shutdown:
    //   1. QTimer stopped on correct thread
    //   2. NativeReaderWorker thread joined
    //   3. BufferPipeline + AI process stopped
    
    if (ctx_to_stop) {
        LOG_INFO("Stopping pipeline for camera {} — waiting for safe shutdown...", camera_id);
        ctx_to_stop.reset(); // Explicit destruction — triggers ~PipelineContext()
        LOG_INFO("Pipeline stopped and destroyed for camera {}", camera_id);
    }
}


void CameraPipelineManager::stopAllPipelines() {
    std::vector<std::unique_ptr<PipelineContext>> stopped_pipes;
    {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        LOG_INFO("Stopping all pipelines...");
        
        for (auto& [id, ctx] : pipelines_) {
            if (ctx) stopped_pipes.push_back(std::move(ctx));
        }
        pipelines_.clear();
    }
    // Destroy all contexts outside lock
    stopped_pipes.clear();
}


bool CameraPipelineManager::isRunning(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (pipelines_.find(camera_id) == pipelines_.end()) return false;
    return pipelines_[camera_id]->running.load();
}

std::optional<std::vector<char>> CameraPipelineManager::getLatestFrame(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (pipelines_.find(camera_id) == pipelines_.end()) return std::nullopt;
    
    auto& ctx = pipelines_[camera_id];
    std::lock_guard<std::mutex> frame_lock(ctx->frame_mutex);
    if (ctx->latest_frame.empty()) return std::nullopt;
    
    return ctx->latest_frame;
}

// ============================================================================
// WORKER THREADS
// ============================================================================




CameraStats CameraPipelineManager::getCameraStats(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    CameraStats stats;

    if (failed_cameras_.count(camera_id)) {
        stats.state = CameraState::FAILED;
        return stats;
    }

    if (pipelines_.find(camera_id) != pipelines_.end()) {
        auto& ctx = pipelines_[camera_id];
        stats.fps = ctx->current_fps.load();
        stats.restart_count = ctx->restart_count.load();
        stats.last_frame_ts = ctx->last_frame_ts.load();
        stats.state = static_cast<CameraState>(ctx->camera_state.load());

        bool thread_running = ctx->running.load();
        if (thread_running && stats.last_frame_ts > 0) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            stats.is_running = (now - stats.last_frame_ts) < 10000;
        } else {
            stats.is_running = false;
        }
    }

    return stats;
}










void CameraPipelineManager::handleH264Data(int camera_id, const QByteArray& data) {}
void CameraPipelineManager::handleLogData(int camera_id, const QByteArray& data) {}
void CameraPipelineManager::handleHlsLogData(int camera_id, const QByteArray& data) {}
void CameraPipelineManager::handleAiLogData(int camera_id, const QByteArray& data) {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (pipelines_.find(camera_id) == pipelines_.end()) return;
    }
    // String parsing outside the lock — harmless; camera_id may have stopped
    // by the time we get here, but the data itself is owned by QByteArray (safe).
    std::string text(data.constData(), data.length());
    if (text.find("BBOX:") != std::string::npos) {
        // Reserved for future bounding-box parsing
    }
}
void CameraPipelineManager::handleFrameReady(int camera_id, const cv::Mat& raw_frame) {
    // Legacy/Raw handler — currently used for snapshots or other raw consumers.
    // Optimization: heavy processing moved to NativeReaderWorker::frameProcessed.
}

void CameraPipelineManager::handleFrameProcessed(int camera_id,
                                                 const std::vector<uchar>& jpeg_data,
                                                 const std::vector<inference::TrackedObject>& objects,
                                                 const nlohmann::json& meta,
                                                 uint64_t timestamp) {
    // ══════════════════════════════════════════════════════════════════
    // LEAN HOT PATH — All heavy lifting (Resize/JPEG/SHM) done by worker!
    //
    // THREAD SAFETY: mutex_ is held for the ENTIRE function body.
    //
    // Rationale: the PipelineContext raw pointer must not be dereferenced
    // after the lock is released, because stopPipeline() on an HTTP thread
    // can erase the pipeline entry and begin destroying the context in the
    // window between unlock and the next ctx access.  Keeping the lock held
    // prevents that race without requiring a shared_ptr refcount on the hot
    // path.  broadcastRawFrame() and processMetadata() do NOT acquire
    // mutex_, so no deadlock is possible.
    // ══════════════════════════════════════════════════════════════════
    // shared_lock: we only read the map to get the ctx pointer.
    // ctx mutations use ctx-level atomics and sub-mutexes (objects_mutex, frame_mutex).
    // stopPipeline (unique_lock) cannot proceed while this shared_lock is held,
    // so ctx remains valid for the entire duration of this function.
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = pipelines_.find(camera_id);
    if (it == pipelines_.end()) return;
    PipelineContext* ctx = it->second.get();

    // 1. Update status — transition to RUNNING on first frame if not already
    ctx->last_frame_ts = timestamp;
    ctx->running = true;
    int prev_state = ctx->camera_state.exchange(static_cast<int>(CameraState::RUNNING));
    if (prev_state != static_cast<int>(CameraState::RUNNING)) {
        LOG_INFO("[STATE] Camera {} → RUNNING (first decoded frame received)", camera_id);
    }

    // 2. Cache objects and metadata for API queries
    {
        std::lock_guard<std::mutex> obj_lock(ctx->objects_mutex);
        ctx->latest_objects = objects;
        ctx->last_metadata_json_ = meta;
    }

    // 3. Cache latest frame for snapshot endpoint
    {
        std::lock_guard<std::mutex> frame_lock(ctx->frame_mutex);
        ctx->latest_frame.assign(jpeg_data.begin(), jpeg_data.end());
    }

    // 4. Broadcast to live-view WebSocket clients (does not acquire mutex_)
    vms::streaming::CameraStreamManager::getInstance().broadcastRawFrame(
        camera_id,
        *reinterpret_cast<const std::vector<uint8_t>*>(&jpeg_data),
        objects, 640, 360
    );

    // 5. Throttled AI event processing (every 500 ms to avoid DB pressure)
    auto steady_now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            steady_now - ctx->last_event_process_time).count() >= 500) {
        AiEventProcessor::getInstance().processMetadata(camera_id, meta, jpeg_data);
        ctx->last_event_process_time = steady_now;
    }
}
// Single global watchdog tick — checks ALL active cameras in one pass.
// Runs on main Qt thread via QTimer (1 s interval). Replaces N per-camera timers.
// Also drives adaptive FPS: cameras with no live WebSocket viewers run at kFpsIdle
// to save CPU; cameras with viewers run at kFpsActive.
void CameraPipelineManager::globalWatchdogTick() {
    // Processing FPS targets. Recording uses raw-packet callback and is unaffected.
    static constexpr int kFpsIdle   = 5;   // no live viewers — AI still runs for event detection
    static constexpr int kFpsActive = 15;  // live viewers — smooth playback

    auto now_sys    = std::chrono::system_clock::now();
    auto now_steady = std::chrono::steady_clock::now();
    uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now_sys.time_since_epoch()).count());

    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (auto& [camera_id, ctx] : pipelines_) {
        if (!ctx) continue;
        if (failed_cameras_.count(camera_id)) continue;

        // ── Adaptive FPS ──────────────────────────────────────────────────────
        if (ctx->worker_ptr) {
            size_t viewers = vms::streaming::CameraStreamManager::getInstance()
                                 .getClientCount(camera_id);
            int target_fps = (viewers > 0) ? kFpsActive : kFpsIdle;
            // Log only on transition to avoid log flood every second.
            int prev_fps = ctx->worker_ptr->getTargetFps();
            if (prev_fps != target_fps) {
                LOG_INFO("[ADAPTIVE-FPS] Camera {} {} viewers → {}fps (was {}fps)",
                         camera_id, viewers, target_fps, prev_fps);
            }
            ctx->worker_ptr->setTargetFps(target_fps);
        }

        // ── Stale-frame watchdog ──────────────────────────────────────────────
        auto uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now_steady - ctx->start_time).count();
        if (uptime_sec < 10) continue; // grace period after startup

        uint64_t last_ts = static_cast<uint64_t>(ctx->last_frame_ts.load());
        if (last_ts == 0 || now_ms < last_ts) continue;

        uint64_t age = now_ms - last_ts;
        if (age > FRAME_TIMEOUT_MS) {
            int prev = ctx->camera_state.exchange(static_cast<int>(CameraState::DEGRADED));
            if (prev != static_cast<int>(CameraState::DEGRADED)) {
                LOG_WARN("[WATCHDOG] [STATE] Camera {} → DEGRADED (no frames for {}ms)", camera_id, age);
            } else {
                LOG_THROTTLED_WARN(30000,
                    "[WATCHDOG] Camera {} still DEGRADED (no frames for {}ms)", camera_id, age);
            }
        }
    }
}

void CameraPipelineManager::handleStreamFailed(int camera_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    LOG_ERROR("[PipelineManager] [STATE] Camera {} → FAILED — all reconnects exhausted. "
              "Watchdog suppressed. Operator must re-enable camera.", camera_id);

    failed_cameras_.insert(camera_id);

    auto it = pipelines_.find(camera_id);
    if (it != pipelines_.end()) {
        auto* ctx = it->second.get();
        ctx->running = false;
        ctx->camera_state = static_cast<int>(CameraState::FAILED);
        // Global watchdog skips failed_cameras_ entries — no per-camera timer to stop.
    }
}

CameraState CameraPipelineManager::getCameraState(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (failed_cameras_.count(camera_id)) return CameraState::FAILED;
    auto it = pipelines_.find(camera_id);
    if (it == pipelines_.end()) return CameraState::FAILED;
    return static_cast<CameraState>(it->second->camera_state.load());
}


std::string CameraPipelineManager::getLatestObjectsJson(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = pipelines_.find(camera_id);
    if (it == pipelines_.end()) return "[]";
    
    auto ctx = it->second.get();
    std::lock_guard<std::mutex> obj_lock(ctx->objects_mutex);
    
    nlohmann::json j = nlohmann::json::array();
    for (const auto& obj : ctx->latest_objects) {
        nlohmann::json o;
        o["track_id"] = obj.track_id;
        o["class_id"] = obj.class_id;
        o["label"] = obj.label;
        o["confidence"] = obj.confidence;
        o["box"] = {obj.bbox.x1, obj.bbox.y1, obj.bbox.x2, obj.bbox.y2};
        j.push_back(o);
    }
    return j.dump();
}

nlohmann::json CameraPipelineManager::getLatestMetadata(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = pipelines_.find(camera_id);
    if (it == pipelines_.end()) return nlohmann::json::object();
    
    auto ctx = it->second.get();
    std::lock_guard<std::mutex> obj_lock(ctx->objects_mutex);
    return ctx->last_metadata_json_;
}

std::string CameraPipelineManager::triggerEventRecording(int camera_id, const std::string& event_id, int duration_sec, int pre_record_sec) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = pipelines_.find(camera_id);
    if (it == pipelines_.end()) return "";
    
    auto ctx = it->second.get();
    if (ctx->buffer_pipeline) {
        return ctx->buffer_pipeline->triggerEvent(event_id, duration_sec, pre_record_sec);
    }
    return "";
}

} // namespace core
} // namespace vms
