#ifndef NOMINMAX
#define NOMINMAX
#endif
// ==============================================================
// File: src/core/camera_pipeline_manager.cpp
// COMPLETE FIXED VERSION with proper FFMPEG cleanup
// ==============================================================

#include <QThread>
#include <QMetaObject>
#include <QObject>
#include <QCoreApplication>
#include <QTimer>
#include "core/native_reader_worker.h"
#include <QByteArray>
#include "streaming/camera_stream_manager_qt.h"
#include "core/camera_pipeline_manager.h"
#include "core/ffmpeg_process.h"
#include "core/frame_bus.h"
#include "core/frame_bus_diagnostics.h"
#include "core/health_monitor.h"
#include "core/media_pipeline.h"
#include "core/pipeline_state_store.h"
#include "core/camera_manager.h"
#include "core/runtime_state.h"
#include "database/models.h"
#include "utils/logger.h"
#include "utils/config.h" // Added for config access
#include "database/camera_repository.h" // Added for camera name lookup
#include <cstdint> // Added for uint64_t
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif
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
#include <cstdio>
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

    std::string selectLiveRtspUrl(const std::string& main_url, const std::string& sub_url) {
        if (!sub_url.empty()) {
            return sub_url;
        }
        return main_url;
    }

    std::string detectAvcCodecString(const QByteArray& packet, const std::string& fallback) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(packet.constData());
        const int size = packet.size();
        for (int i = 0; i + 8 < size; ++i) {
            int start_code_len = 0;
            if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
                start_code_len = 3;
            } else if (i + 4 < size && bytes[i] == 0 && bytes[i + 1] == 0 &&
                       bytes[i + 2] == 0 && bytes[i + 3] == 1) {
                start_code_len = 4;
            }
            if (start_code_len == 0) {
                continue;
            }
            const int nal_index = i + start_code_len;
            if ((bytes[nal_index] & 0x1F) != 7 || nal_index + 3 >= size) {
                continue;
            }
            char codec[16];
            std::snprintf(codec, sizeof(codec), "avc1.%02X%02X%02X",
                          bytes[nal_index + 1], bytes[nal_index + 2], bytes[nal_index + 3]);
            return codec;
        }
        return fallback;
    }
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

    // Shared Memory Manager
    std::unique_ptr<::ipc::SharedMemoryManager> shm_manager;

    // Phase B (2026-05-07): the BufferPipeline + MediaMtxPublisher + ContinuousRecorder
    // trio used to live as 3 raw unique_ptrs here. They are now grouped behind
    // MediaPipeline so PipelineContext stays focused on the inference/IO path
    // and lifecycle ordering for the media subsystems is owned in one place.
    std::unique_ptr<vms::core::MediaPipeline> media;

    // Phase B: ctx-local caches for latest_objects/latest_frame/last_metadata_json_
    // were removed in favor of PipelineStateStore as the single source of truth.

    std::atomic<int64_t> last_frame_ts{0};
    std::atomic<double> current_fps{0.0};
    std::atomic<int> restart_count{0};
    std::atomic<int> no_frame_count{0};
    std::atomic<int> camera_state{static_cast<int>(CameraState::CONNECTING)};

    // BUG-PM-RESTART-01: AI worker subprocess restart-on-crash state.
    // ai_cmd_cache stores the final spawn command string so a respawn after
    // crash can re-run start() without re-deriving paths/json. ai_restart_count
    // bounds the retry loop. ai_last_restart_ms is the steady-clock epoch of
    // the last respawn attempt; we use it to age out the count after a calm
    // 10-minute window (so a flaky camera doesn't permanently exhaust its
    // budget after a single bad day).
    std::string ai_cmd_cache;
    std::atomic<int> ai_restart_count{0};
    std::atomic<int64_t> ai_last_restart_ms{0};
    
    std::chrono::steady_clock::time_point last_log_time;
    std::chrono::steady_clock::time_point last_ai_log_time;          // PERF: throttle AI detection logging
    std::chrono::steady_clock::time_point last_event_process_time;   // PERF: throttle AiEventProcessor
    std::chrono::steady_clock::time_point fps_window_start;
    uint32_t fps_window_frames{0};
    uint64_t frame_count_{0};                                         // PERF: per-camera frame counter

    std::string rtsp_url;
    std::string sub_stream_url;
    std::string live_rtsp_url;
    bool use_h264_live_ws{false};
    std::string h264_codec{"avc1.42E01E"};
    std::string camera_name;
    bool using_backup_stream = false;
    std::chrono::steady_clock::time_point start_time;

    // ZmqEventBridge metadata subscription. The connection's receiver context is
    // qApp (lives forever), so without an explicit disconnect each camera restart
    // would add another lambda — after N restarts the ZMQ event would be processed
    // N times, inflating CPU and double-writing PipelineStateStore. Tracking the
    // handle here lets ~PipelineContext() detach exactly its own subscription.
    QMetaObject::Connection metadata_subscription;

    // Explicit destructor: enforce safe shutdown order (no per-camera QTimer anymore;
    // global watchdog in CameraPipelineManager handles all cameras).
    ~PipelineContext() {
        should_stop = true;
        running = false;

        // 0. Drop the ZMQ metadata subscription before anything else so the lambda
        //    cannot fire mid-teardown and observe half-destroyed state. Safe to call
        //    even if the connection was never established (default-constructed handle
        //    yields a no-op disconnect).
        if (metadata_subscription) {
            QObject::disconnect(metadata_subscription);
        }

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

        // 3. Tear down media subsystems (BufferPipeline + ContinuousRecorder +
        //    MediaMtxPublisher). MediaPipeline::stop() preserves the legacy
        //    ordering buffer → continuous_recorder → mediamtx; this reset()
        //    triggers stop() then destruction.
        media.reset();

        // 4. Stop AI subprocess + reader's FFmpeg process.
        if (ai_process) ai_process->stop();
        if (process)    process->stop();

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
    HealthMonitor::getInstance().start([this]() { globalWatchdogTick(); });
}

void CameraPipelineManager::startAllPipelines() {
    LOG_INFO("Auto-starting all camera pipelines...");
    auto& camera_manager = vms::core::CameraManager::getInstance();
    auto cameras = camera_manager.getAllCameras();
    
    for (const auto& cam : cameras) {
        if (cam.is_active && !cam.rtsp_url.empty()) {
            LOG_INFO("Auto-starting camera {}: {}", cam.id, cam.name);
            startPipeline(cam.id, cam.rtsp_url);
        }
    }
}

CameraPipelineManager::~CameraPipelineManager() {
    HealthMonitor::getInstance().stop();
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

    PipelineStateStore::getInstance().registerCamera(camera_id);
    HealthMonitor::getInstance().registerCamera(camera_id);
    HealthMonitor::getInstance().clearFailure(camera_id);
    // Subscribe diagnostics so the FrameBus contract is exercised end-to-end on every
    // camera. Cheap onFrame (one relaxed atomic + throttled log) — does not affect
    // the producer hot path measurably.
    FrameBusDiagnostics::getInstance().attach(camera_id);
    std::string sub_url = camera_opt->sub_stream_url;
    std::string live_url = selectLiveRtspUrl(rtsp_url, sub_url);
    std::string camera_name_cached = camera_opt->name.empty() ? "Camera " + std::to_string(camera_id) : camera_opt->name;

    auto ctx = std::make_unique<PipelineContext>();
    ctx->camera_name = camera_name_cached;
    ctx->rtsp_url = rtsp_url;
    ctx->sub_stream_url = sub_url;
    ctx->live_rtsp_url = live_url;
    // Raw H264-over-WS is too fragile across heterogeneous RTSP cameras
    // (packetization/profile/timestamp variance). Keep RTSP on the stable
    // JPEG live path for now; sub-stream selection still removes most of the
    // previous decode cost.
    ctx->use_h264_live_ws = false;
    
    // BUG 3 FIX: Initialize last_frame_ts to current system time to avoid immediate watchdog timeout.
    auto now_init = std::chrono::system_clock::now();
    ctx->last_frame_ts = std::chrono::duration_cast<std::chrono::milliseconds>(now_init.time_since_epoch()).count();
    // Record pipeline start time for watchdog grace period
    ctx->start_time = std::chrono::steady_clock::now();
    ctx->fps_window_start = ctx->start_time;
    PipelineStateStore::getInstance().updateStats(camera_id, 0.0, 0, ctx->last_frame_ts.load(),
                                                  CameraState::CONNECTING, true);
    HealthMonitor::getInstance().updateFrameHeartbeat(camera_id, static_cast<uint64_t>(ctx->last_frame_ts.load()));
    HealthMonitor::getInstance().setState(camera_id, CameraState::CONNECTING);

    if (live_url != rtsp_url) {
        LOG_INFO("[Manager] Camera {} live pipeline using sub stream: {}", camera_id, live_url);
    } else {
        LOG_INFO("[Manager] Camera {} live pipeline using primary stream", camera_id);
    }
    
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

        // Windows CommandLineToArgvW: inside a "..."-quoted argument, an embedded
        // double-quote must be written as \". A backslash followed by a quote is
        // an escaped quote; a backslash NOT followed by a quote is a literal \.
        // The previous version wrote `"\""` (== "), which left every JSON quote
        // unescaped → cmd-line tokenization broke on the first `:` and ai_worker
        // received "{":<...>" as separate tokens, fell through to the catch-all
        // and ran with default config → user-set face_match_threshold ignored.
        std::string escaped_json;
        escaped_json.reserve(ai_config_json.size() + 8);
        for (char c : ai_config_json) {
            if (c == '\\') {
                escaped_json += "\\\\";
            } else if (c == '"') {
                escaped_json += "\\\"";
            } else {
                escaped_json += c;
            }
        }

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

        // Cache for BUG-PM-RESTART-01 respawn path. Stored before start() so
        // even if the first launch fails the cache is populated and a future
        // restart attempt has the right string to retry.
        ctx->ai_cmd_cache = ai_cmd;

        QObject::connect(ctx->ai_process.get(), &vms::core::FFmpegProcess::stdoutReady, ctx->ai_process.get(),
                         [this, camera_id](const QByteArray& data) { handleAiLogData(camera_id, data); });
        QObject::connect(ctx->ai_process.get(), &vms::core::FFmpegProcess::stderrReady, ctx->ai_process.get(),
                         [this, camera_id](const QByteArray& data) { handleAiLogData(camera_id, data); });
        // BUG-PM-RESTART-01: pre-fix nothing listened to processStopped — when
        // the worker exited (BUG-AIW-LOOP-01 50-failure bail, segfault, OS
        // kill, or any other reason), the camera silently lost AI detection
        // until manual stop/start. Now we log the death and schedule a
        // respawn with backoff. Receiver context is the ai_process instance
        // itself so the connection is auto-disposed when the FFmpegProcess
        // is destroyed in ~PipelineContext.
        QObject::connect(ctx->ai_process.get(), &vms::core::FFmpegProcess::processStopped, ctx->ai_process.get(),
                         [this, camera_id](int code) { onAiWorkerStopped(camera_id, code); });
        
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

    // ── MediaPipeline construction + Phase 1 (BufferPipeline) ─────────────
    // Phases 2 and 3 (MediaMTX publisher / ContinuousRecorder) come later in
    // this function to preserve the original lifecycle ordering.
    {
        vms::core::MediaPipeline::Config mcfg;
        mcfg.camera_id        = camera_id;
        mcfg.live_url         = live_url;
        mcfg.recording_url    = sub_url.empty() ? rtsp_url : sub_url;
        mcfg.segment_seconds  = 60;
        mcfg.retention_days   = 7;
        mcfg.mediamtx_enabled = (qEnvironmentVariable("VMS_ENABLE_MEDIAMTX", "0") == QStringLiteral("1"));
        mcfg.mediamtx_url     = qEnvironmentVariable("VMS_MEDIAMTX_URL", "").toStdString();
        ctx->media = std::make_unique<vms::core::MediaPipeline>(std::move(mcfg));
    }
    ctx->media->startBuffer();

    // The stream will be opened by NativeReaderWorker::run() in its own thread.
    
    auto* worker = new NativeReaderWorker(camera_id, live_url);
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
                             if (raw_ctx->media) {
                                 raw_ctx->media->writeRawData(
                                     reinterpret_cast<const uint8_t*>(data.constData()),
                                     static_cast<std::size_t>(data.size()));
                             }
                             if (raw_ctx->use_h264_live_ws &&
                                 vms::streaming::CameraStreamManager::getInstance().getClientCount(camera_id) > 0) {
                                 if (isKeyframe) {
                                     raw_ctx->h264_codec = detectAvcCodecString(data, raw_ctx->h264_codec);
                                 }
                                 // Phase B: read latest objects from PipelineStateStore (single
                                 // source of truth). Previously dual-read from ctx->latest_objects.
                                 // The store's shared_mutex grants concurrent reads under load.
                                 auto objects = PipelineStateStore::getInstance().latestObjects(camera_id);
                                 const uint64_t now_us = static_cast<uint64_t>(
                                     std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::system_clock::now().time_since_epoch()).count());
                                 vms::streaming::CameraStreamManager::getInstance().broadcastH264Frame(
                                     camera_id,
                                     data,
                                     objects,
                                     now_us,
                                     isKeyframe,
                                     raw_ctx->h264_codec,
                                     "",
                                     static_cast<int>(FRAME_WIDTH),
                                     static_cast<int>(FRAME_HEIGHT));
                             }
                         }
                     }, Qt::QueuedConnection);
                     
    // FaceID Phase 2: Metadata update from ZmqEventBridge.
    // BUG-CPL-01 FIX: stash the connection handle so ~PipelineContext() can
    // disconnect it on stopPipeline()/restart. Receiver context stays qApp so
    // the lambda runs on the Qt main thread (consistent with the rest of the
    // pipeline's signal model); without the explicit disconnect the lambda
    // outlived the PipelineContext and accumulated one extra duplicate handler
    // per restart.
    ctx->metadata_subscription = QObject::connect(
        &vms::ipc::ZmqEventBridge::getInstance(), &vms::ipc::ZmqEventBridge::eventReceived, qApp,
        [this, camera_id](const QString& type, const QString& raw_json) {
            if (type == "metadata" || type == "METADATA") {
                // Phase B: pipelines_ membership check still uses the manager's
                // shared mutex; the actual write goes only to PipelineStateStore.
                {
                    std::shared_lock<std::shared_mutex> lock(mutex_);
                    if (pipelines_.find(camera_id) == pipelines_.end()) return;
                }
                try {
                    auto j = nlohmann::json::parse(raw_json.toStdString());
                    if (j.value("camera_id", -1) == camera_id) {
                        PipelineStateStore::getInstance().updateMetadata(camera_id, j);
                    }
                } catch (...) {}
            }
        });
                     
    // ── MediaPipeline Phase 2 (MediaMTX publisher) ────────────────────────
    // Opt-in via `VMS_ENABLE_MEDIAMTX=1` (config bridged into MediaPipeline::Config
    // above). Skipped by default because without a MediaMTX server on
    // localhost:8554 FFmpeg's RTSP output blocks → stdin backs up → 100s of
    // dropped frames → kill+restart loop. MediaPipeline owns the moveToThread +
    // start-on-Qt-thread dance and logs success/disabled/failure internally.
    // connectToWorker() is a no-op when the publisher isn't running.
    ctx->media->startMediaMtx();
    ctx->media->connectToWorker(worker);

    // [OPTIMIZATION] Set SHM manager and connect processed signal
    worker->setShmManager(ctx->shm_manager.get());
    QObject::connect(worker, &vms::core::NativeReaderWorker::frameProcessed, qApp,
                     [this](int camera_id, const cv::Mat& frame,
                            const std::vector<uchar>& jpeg_data,
                            const std::vector<inference::TrackedObject>& objects,
                            const nlohmann::json& meta, uint64_t timestamp) {
                         handleFrameProcessed(camera_id, frame, jpeg_data, objects, meta, timestamp);
                     }, Qt::QueuedConnection);

    // Permanent failure signal — stops watchdog from restarting a dead camera
    QObject::connect(worker, &vms::core::NativeReaderWorker::streamFailed, qApp,
                     [this, camera_id]() { handleStreamFailed(camera_id); },
                     Qt::QueuedConnection);

    worker->start();

    // [OPTIMIZATION] Staggered startup to avoid connection flood to Dahua/Hikvision cameras.
    // The ContinuousRecorder spins its own RTSP-pulling FFmpeg, so opening it
    // immediately after worker->start() risks two simultaneous connections to
    // the same NVR. The 500ms sleep keeps the legacy ordering even when the
    // recording URL is the main stream (sub_url empty).
    QThread::msleep(500);

    // ── MediaPipeline Phase 3 (ContinuousRecorder, 24/7 segment-based) ────
    // Uses sub_stream_url when present (avoids Dahua TCP drop from concurrent
    // main-stream connections); falls back to main RTSP. Config wired in the
    // MediaPipeline construction site at the top of this function.
    ctx->media->startContinuousRecorder();
    
    // BUG 3 FIX: Mark pipeline as running immediately to prevent supervisor restarts.
    ctx->running = true;
    LOG_INFO("[Manager] Pipeline {} marked as running with last_frame_ts = {}", camera_id, ctx->last_frame_ts.load());
    
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

    FrameBusDiagnostics::getInstance().detach(camera_id);
    FrameBus::getInstance().unsubscribeAll(camera_id);
    PipelineStateStore::getInstance().removeCamera(camera_id);
    HealthMonitor::getInstance().unregisterCamera(camera_id);
}


void CameraPipelineManager::stopAllPipelines() {
    std::vector<std::unique_ptr<PipelineContext>> stopped_pipes;
    std::vector<int> camera_ids;
    {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        LOG_INFO("Stopping all pipelines...");
        
        for (auto& [id, ctx] : pipelines_) {
            if (ctx) {
                camera_ids.push_back(id);
                stopped_pipes.push_back(std::move(ctx));
            }
        }
        pipelines_.clear();
    }
    // Destroy all contexts outside lock
    stopped_pipes.clear();

    for (int camera_id : camera_ids) {
        FrameBusDiagnostics::getInstance().detach(camera_id);
        FrameBus::getInstance().unsubscribeAll(camera_id);
        PipelineStateStore::getInstance().removeCamera(camera_id);
        HealthMonitor::getInstance().unregisterCamera(camera_id);
    }
}


bool CameraPipelineManager::isRunning(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (pipelines_.find(camera_id) == pipelines_.end()) return false;
    return pipelines_[camera_id]->running.load();
}

std::optional<std::vector<char>> CameraPipelineManager::getLatestFrame(int camera_id) {
    return PipelineStateStore::getInstance().latestFrameJpeg(camera_id);
}

// ============================================================================
// WORKER THREADS
// ============================================================================




CameraStats CameraPipelineManager::getCameraStats(int camera_id) {
    CameraStats stats;
    auto snapshot = PipelineStateStore::getInstance().snapshot(camera_id);
    if (!snapshot) {
        stats.state = CameraState::FAILED;
        return stats;
    }

    stats.is_running = snapshot->is_running;
    stats.fps = snapshot->fps;
    stats.restart_count = snapshot->restart_count;
    stats.last_frame_ts = snapshot->last_frame_ts;
    stats.state = snapshot->state;
    stats.cpu_usage_percent = snapshot->cpu_usage_percent;
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
    std::string text(data.constData(), data.length());
    if (text.empty()) return;

    // Forward AI worker stdout/stderr to the main logger so model-load failures,
    // exceptions, and FPS reports are actually visible. The worker emits one JSON
    // line per inferred frame on stdout (very high volume) — drop those by default
    // since they contain only `frame_id`/`objects` and the [AI-DIAG] log already
    // surfaces detection counts on the SHM consumer side.
    //
    // Heuristics:
    //   - Lines beginning with `{"frame_id"` are per-frame inference dumps → drop
    //   - Lines containing "Loading", "Error", "Warning", "Loaded", "FPS:", "Engine"
    //     etc. are diagnostic → forward at INFO/WARN
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        // Strip trailing CR (Windows pipes)
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;

        // Drop high-volume per-frame JSON
        if (line.size() > 1 && line.front() == '{' && line.find("\"frame_id\"") != std::string::npos) continue;

        // Drop per-inference diagnostic spam — these fire 5-25× per second per camera
        // and were causing ~1000 LOG_INFO calls/s under 3 cameras, contributing to
        // visible live-stream stutter. Keep ai_worker's own [AI-DIAG] every 10s for
        // health, plus result.objects= every 30 frames for pipeline tracing.
        if (line.find("[AdvancedInfer] YOLO output size") != std::string::npos ||
            line.find("[AdvancedInfer] YOLO layout") != std::string::npos ||
            line.find("[SCRFD] Input:") != std::string::npos ||
            line.find("[SCRFD]   output[") != std::string::npos ||
            line.find("[SCRFD] stride=") != std::string::npos ||
            line.find("[SCRFD] max_score=") != std::string::npos ||
            line.find("[SHM] ") != std::string::npos) {
            continue;
        }

        // Anything else → forward. Worker logs to stderr by convention, so include
        // both streams uniformly here. This is throttled by line frequency rather
        // than time so model-load output is preserved.
        if (line.find("Error") != std::string::npos ||
            line.find("ERROR") != std::string::npos ||
            line.find("Failed") != std::string::npos ||
            line.find("Exception") != std::string::npos) {
            LOG_WARN("[AI-Worker-{}] {}", camera_id, line);
        } else {
            LOG_INFO("[AI-Worker-{}] {}", camera_id, line);
        }
    }
}
void CameraPipelineManager::handleFrameReady(int camera_id, const cv::Mat& raw_frame) {
    // Legacy/Raw handler — currently used for snapshots or other raw consumers.
    // Optimization: heavy processing moved to NativeReaderWorker::frameProcessed.
}

void CameraPipelineManager::handleFrameProcessed(int camera_id,
                                                 const cv::Mat& frame,
                                                 const std::vector<uchar>& jpeg_data,
                                                 const std::vector<inference::TrackedObject>& objects,
                                                 const nlohmann::json& meta,
                                                 uint64_t timestamp) {
    // ══════════════════════════════════════════════════════════════════
    // LEAN HOT PATH — Worker already resized frame + updated SHM.
    // JPEG is now encoded only for webcam WS or on-demand HTTP/snapshot calls.
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
    // ctx mutations use ctx-level atomics; latest frame/objects/metadata are
    // routed to PipelineStateStore (single source of truth, see Phase B).
    // stopPipeline (unique_lock) cannot proceed while this shared_lock is held,
    // so ctx remains valid for the entire duration of this function.
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = pipelines_.find(camera_id);
    if (it == pipelines_.end()) return;
    PipelineContext* ctx = it->second.get();
    const uint64_t frame_index = ++ctx->frame_count_;

    // 1. Update status — transition to RUNNING on first frame if not already
    ctx->last_frame_ts = timestamp;
    ctx->running = true;
    ctx->fps_window_frames++;
    auto fps_now = std::chrono::steady_clock::now();
    auto fps_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        fps_now - ctx->fps_window_start).count();
    if (fps_elapsed_ms >= 1000) {
        ctx->current_fps = (ctx->fps_window_frames * 1000.0) / static_cast<double>(fps_elapsed_ms);
        ctx->fps_window_frames = 0;
        ctx->fps_window_start = fps_now;
    }
    int prev_state = ctx->camera_state.exchange(static_cast<int>(CameraState::RUNNING));
    if (prev_state != static_cast<int>(CameraState::RUNNING)) {
        LOG_INFO("[STATE] Camera {} → RUNNING (first decoded frame received)", camera_id);
    }
    HealthMonitor::getInstance().updateFrameHeartbeat(camera_id, timestamp);
    HealthMonitor::getInstance().setState(camera_id, CameraState::RUNNING);
    // Phase B: PipelineStateStore is the single source of truth for the latest
    // frame, objects, and metadata. The dual-write to ctx->latest_objects /
    // ctx->latest_frame / ctx->last_metadata_json_ has been removed (all readers
    // now go through the store).
    PipelineStateStore::getInstance().updateFrame(camera_id, jpeg_data, objects, meta, timestamp);
    PipelineStateStore::getInstance().updateStats(camera_id,
                                                  ctx->current_fps.load(),
                                                  ctx->restart_count.load(),
                                                  ctx->last_frame_ts.load(),
                                                  CameraState::RUNNING,
                                                  true);

    // 2. WebSocket broadcast is now handled directly in NativeReaderWorker::processAndEmit
    // (called from worker thread before emitting this signal) to eliminate the Qt event
    // loop hop latency (~15ms on Windows). Nothing to do here for JPEG streams.

    // 3. Throttled AI event processing (every 500 ms to avoid DB pressure).
    // Skip when frame is empty (worker only sends a real Mat once per ~500ms; on
    // the lightweight emits we still updated counters/objects above but have no
    // image to crop snapshots from).
    if (!frame.empty()) {
        auto steady_now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                steady_now - ctx->last_event_process_time).count() >= 500) {
            AiEventProcessor::getInstance().processMetadata(camera_id, meta, frame);
            ctx->last_event_process_time = steady_now;
        }
    }

    if (FrameBus::getInstance().subscriberCount(camera_id) > 0) {
        FrameEnvelope envelope;
        envelope.camera_id = camera_id;
        envelope.timestamp_ms = timestamp;
        envelope.frame_index = frame_index;
        if (!frame.empty()) {
            // Zero-copy fanout: cv::Mat header copy here is cheap (refcount++ on the
            // underlying pixel buffer). The shared_ptr keeps the cv::Mat alive across
            // any number of consumers without an extra pixel deep-copy per consumer.
            envelope.raw_frame = std::make_shared<const cv::Mat>(frame);
        }
        envelope.jpeg_preview = jpeg_data;
        envelope.objects = objects;
        envelope.metadata = meta;
        envelope.source_stream = ctx->live_rtsp_url;
        envelope.is_backup_stream = ctx->using_backup_stream;
        FrameBus::getInstance().publish(camera_id, envelope);
    }
}
// Single global watchdog tick — checks ALL active cameras in one pass.
// Runs on main Qt thread via QTimer (1 s interval). Replaces N per-camera timers.
// Also drives adaptive FPS: cameras with no live WebSocket viewers run at kFpsIdle
// to save CPU; cameras with viewers run at kFpsActive.
// Read cumulative CPU time (kernel + user) for a child process in nanoseconds.
// Returns false if the PID is invalid or the OS rejected the query.
namespace {
bool sampleProcessCpuNs(long long pid, uint64_t& out_ns) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    FILETIME ft_create, ft_exit, ft_kernel, ft_user;
    BOOL ok = GetProcessTimes(h, &ft_create, &ft_exit, &ft_kernel, &ft_user);
    CloseHandle(h);
    if (!ok) return false;
    auto to_uint64 = [](const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    // FILETIME is in 100-ns units → multiply by 100 for ns.
    out_ns = (to_uint64(ft_kernel) + to_uint64(ft_user)) * 100ull;
    return true;
#else
    std::ifstream fs("/proc/" + std::to_string(pid) + "/stat");
    if (!fs) return false;
    std::string content;
    std::getline(fs, content);
    // Skip past comm field which is parenthesized and may contain spaces.
    auto rparen = content.rfind(')');
    if (rparen == std::string::npos) return false;
    std::istringstream rest(content.substr(rparen + 1));
    std::string state_field;
    rest >> state_field;
    // Skip fields 4..13 (1-indexed in proc(5); we already consumed 1=pid, 2=comm, 3=state).
    for (int i = 0; i < 10; ++i) { unsigned long long tmp; if (!(rest >> tmp)) return false; }
    unsigned long long utime = 0, stime = 0;
    if (!(rest >> utime >> stime)) return false;
    static const long ticks_per_sec = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    out_ns = static_cast<uint64_t>((utime + stime) * (1'000'000'000ull / static_cast<uint64_t>(ticks_per_sec)));
    return true;
#endif
}

int hostCpuCount() {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return std::max<int>(1, static_cast<int>(si.dwNumberOfProcessors));
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return static_cast<int>(n > 0 ? n : 1);
#endif
}
} // namespace

void CameraPipelineManager::globalWatchdogTick() {
    // Processing FPS targets. Recording uses raw-packet callback and is unaffected.
    // 2026-04-26: hạ kFpsActive 25→15. 3 cam × 25fps × encode JPEG + WS broadcast +
    // SHM read/write + AI inference @ 5-25fps là quá tải CPU dù GPU rảnh, gây
    // visible stutter. 15fps đủ smooth cho live monitoring và giảm 40% CPU load.
    // User có thể tăng lại nếu thấy mượt và GPU/CPU dư.
    static constexpr int kFpsIdle   = 5;   // no live viewers — AI still runs for event detection
    static constexpr int kFpsActive = 15;  // live viewers — balance smooth vs CPU

    auto now_sys    = std::chrono::system_clock::now();
    auto now_steady = std::chrono::steady_clock::now();
    uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now_sys.time_since_epoch()).count());

    std::shared_lock<std::shared_mutex> lock(mutex_);

    // Track which PIDs we touched this tick so stale cache entries can be pruned.
    std::unordered_set<long long> live_pids;
    const int n_cores = hostCpuCount();

    for (auto& [camera_id, ctx] : pipelines_) {
        if (!ctx) continue;
        if (failed_cameras_.count(camera_id)) continue;

        // ── Per-pipeline CPU% (FFmpeg + ai_worker child processes) ────────────
        // Computed at 1 Hz here, published into PipelineStateStore so HTTP
        // /api/cameras can read it cheaply.
        {
            std::vector<long long> child_pids;
            if (ctx->process)    child_pids.push_back(ctx->process->processId());
            if (ctx->ai_process) child_pids.push_back(ctx->ai_process->processId());

            double pipeline_cpu_pct = 0.0;
            bool   have_sample      = false;
            for (long long pid : child_pids) {
                if (pid <= 0) continue;
                live_pids.insert(pid);
                uint64_t cpu_ns_now = 0;
                if (!sampleProcessCpuNs(pid, cpu_ns_now)) continue;
                auto& s = cpu_sample_cache_[pid];
                if (s.initialized) {
                    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       now_steady - s.sampled_at).count();
                    if (wall_ns > 0 && cpu_ns_now >= s.cpu_time_ns) {
                        const double cpu_diff_ns = static_cast<double>(cpu_ns_now - s.cpu_time_ns);
                        // % of one core, then divide by core count to express as
                        // fraction of total host CPU (matches Windows Task Manager
                        // and Linux `top -1 i` per-process display normalised).
                        const double pct_of_one_core = cpu_diff_ns / static_cast<double>(wall_ns) * 100.0;
                        pipeline_cpu_pct += pct_of_one_core / static_cast<double>(n_cores);
                        have_sample = true;
                    }
                }
                s.cpu_time_ns  = cpu_ns_now;
                s.sampled_at   = now_steady;
                s.initialized  = true;
            }

            if (have_sample) {
                pipeline_cpu_pct = std::max(0.0, std::min(100.0, pipeline_cpu_pct));
                PipelineStateStore::getInstance().updateCpuUsage(camera_id, pipeline_cpu_pct);
            }
        }

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
                FrameBus::getInstance().publishStateChange(camera_id, CameraState::DEGRADED);
            } else {
                LOG_THROTTLED_WARN(30000,
                    "[WATCHDOG] Camera {} still DEGRADED (no frames for {}ms)", camera_id, age);
            }
            HealthMonitor::getInstance().setState(camera_id, CameraState::DEGRADED);
            PipelineStateStore::getInstance().updateStats(camera_id,
                                                          ctx->current_fps.load(),
                                                          ctx->restart_count.load(),
                                                          ctx->last_frame_ts.load(),
                                                          CameraState::DEGRADED,
                                                          ctx->running.load());
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

    HealthMonitor::getInstance().markPermanentFailure(
        camera_id, "All reconnect attempts exhausted");
    PipelineStateStore::getInstance().setState(
        camera_id, CameraState::FAILED, "All reconnect attempts exhausted");
    FrameBus::getInstance().publishStateChange(camera_id, CameraState::FAILED);
}

// BUG-PM-RESTART-01 (audit 2026-05-09): runs on the Qt main thread (because
// it's invoked from a signal whose receiver is the FFmpegProcess QObject,
// which lives on the main thread). Bounded backoff so a permanently-broken
// model file (e.g., truncated mid-deploy) doesn't infinite-restart at high
// rate and burn CPU + log volume.
void CameraPipelineManager::onAiWorkerStopped(int camera_id, int exit_code) {
    if (vms::core::shutting_down.load(std::memory_order_acquire)) {
        return;
    }

    // Backoff schedule (ms): 5s, 15s, 60s, 300s, then 600s steady.
    static constexpr int kBackoffMs[] = {5000, 15000, 60000, 300000, 600000};
    static constexpr int kMaxRestarts = 5;
    // After this many milliseconds of no restart attempts, the counter ages
    // back to 0 — a flaky day shouldn't permanently exhaust the budget.
    static constexpr int64_t kCounterResetWindowMs = 10 * 60 * 1000;

    int attempt = -1;
    int delay_ms = 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = pipelines_.find(camera_id);
        if (it == pipelines_.end()) {
            LOG_INFO("[Manager] AI Worker for camera {} stopped (exit={}); pipeline already gone — not restarting",
                     camera_id, exit_code);
            return;
        }
        auto* ctx = it->second.get();

        // If the pipeline is being torn down (stopPipeline already called),
        // don't fight the teardown by respawning. should_stop is the right
        // signal — it's set in ~PipelineContext too.
        if (ctx->should_stop.load(std::memory_order_acquire) ||
            !ctx->running.load(std::memory_order_acquire)) {
            LOG_INFO("[Manager] AI Worker for camera {} stopped (exit={}); pipeline shutting down — not restarting",
                     camera_id, exit_code);
            return;
        }

        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t last_ms = ctx->ai_last_restart_ms.load(std::memory_order_acquire);
        if (last_ms > 0 && (now_ms - last_ms) > kCounterResetWindowMs) {
            ctx->ai_restart_count.store(0, std::memory_order_release);
        }
        attempt = ctx->ai_restart_count.fetch_add(1, std::memory_order_acq_rel);
        ctx->ai_last_restart_ms.store(now_ms, std::memory_order_release);

        if (attempt >= kMaxRestarts) {
            LOG_ERROR("[Manager] AI Worker for camera {} has restarted {} times in 10 min — giving up. "
                      "Operator must restart the camera manually.",
                      camera_id, attempt);
            return;
        }
        delay_ms = kBackoffMs[std::min<size_t>(attempt, sizeof(kBackoffMs)/sizeof(int) - 1)];
    }

    LOG_WARN("[Manager] AI Worker for camera {} exited (code={}); scheduling restart attempt {} in {}ms",
             camera_id, exit_code, attempt + 1, delay_ms);

    // Schedule on the Qt main thread; FFmpegProcess::start() asserts thread
    // affinity. qApp lives forever so the timer can't outlive its parent.
    QTimer::singleShot(delay_ms, qApp, [this, camera_id]() {
        respawnAiWorkerOnQtThread(camera_id);
    });
}

void CameraPipelineManager::respawnAiWorkerOnQtThread(int camera_id) {
    if (vms::core::shutting_down.load(std::memory_order_acquire)) {
        return;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = pipelines_.find(camera_id);
    if (it == pipelines_.end()) {
        LOG_INFO("[Manager] AI Worker respawn for camera {}: pipeline gone, skipping", camera_id);
        return;
    }
    auto* ctx = it->second.get();
    if (ctx->should_stop.load(std::memory_order_acquire) ||
        !ctx->running.load(std::memory_order_acquire)) {
        LOG_INFO("[Manager] AI Worker respawn for camera {}: pipeline shutting down, skipping", camera_id);
        return;
    }
    if (!ctx->ai_process || ctx->ai_cmd_cache.empty()) {
        LOG_WARN("[Manager] AI Worker respawn for camera {}: no cached cmd or process, skipping", camera_id);
        return;
    }

    LOG_INFO("[Manager] Respawning AI Worker for camera {}", camera_id);
    bool started = ctx->ai_process->start(ctx->ai_cmd_cache);
    if (!started) {
        LOG_ERROR("[Manager] AI Worker respawn for camera {} failed at start()", camera_id);
        // The new failure will trip processStopped again and re-enter the
        // backoff path; no need to manually re-schedule here.
    } else {
        LOG_INFO("[Manager] AI Worker respawn for camera {} succeeded", camera_id);
    }
}

CameraState CameraPipelineManager::getCameraState(int camera_id) {
    auto snapshot = PipelineStateStore::getInstance().snapshot(camera_id);
    if (!snapshot) return CameraState::FAILED;
    return snapshot->state;
}


std::string CameraPipelineManager::getLatestObjectsJson(int camera_id) {
    auto objects = PipelineStateStore::getInstance().latestObjects(camera_id);
    nlohmann::json j = nlohmann::json::array();
    for (const auto& obj : objects) {
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
    return PipelineStateStore::getInstance().latestMetadata(camera_id);
}

std::string CameraPipelineManager::triggerEventRecording(int camera_id, const std::string& event_id, int duration_sec, int pre_record_sec) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = pipelines_.find(camera_id);
    if (it == pipelines_.end()) return "";
    
    auto ctx = it->second.get();
    if (ctx->media) {
        return ctx->media->triggerEvent(event_id, duration_sec, pre_record_sec);
    }
    return "";
}

} // namespace core
} // namespace vms
