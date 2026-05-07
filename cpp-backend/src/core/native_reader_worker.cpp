#include "core/native_reader_worker.h"
#include "utils/logger.h"
#include <algorithm>
#include <chrono>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <QtConcurrent>
#include "ai/ipc/shared_memory_manager.h"
#include "inference/multi_model_infer.h"
#include "streaming/camera_stream_manager_qt.h"
#include <nlohmann/json.hpp>

namespace {
// INTER_NEAREST processes only dst_pixels (230K at 640×360) regardless of src size.
// For a 3072×2048 source that's 27x fewer pixel ops than INTER_AREA/LINEAR.
// Quality is fine for a live-view thumbnail.
inline cv::Mat fastDownscale(const cv::Mat& src, int w, int h) {
    if (src.cols == w && src.rows == h) return src;
    cv::Mat dst;
    cv::resize(src, dst, cv::Size(w, h), 0, 0, cv::INTER_NEAREST);
    return dst;
}
} // namespace

namespace vms {
namespace core {

NativeReaderWorker::NativeReaderWorker(int camera_id, const std::string& url, QObject* parent)
    : QThread(parent), camera_id_(camera_id), url_(url) {
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<std::vector<unsigned char>>("std::vector<unsigned char>");
    qRegisterMetaType<std::vector<inference::TrackedObject>>("std::vector<inference::TrackedObject>");
    qRegisterMetaType<nlohmann::json>("nlohmann::json");

    // Two dedicated threads per camera so the next frame's JPEG encode + SHM read
    // can begin while the previous frame is still being broadcast over WebSocket.
    // The drop-if-busy gate in run() (`processing_future_.isFinished() || !isRunning()`)
    // already prevents queue buildup; raising the pool from 1 → 2 just allows pipelining
    // and roughly doubles steady-state throughput at 25fps without piling memory.
    processing_pool_.setMaxThreadCount(2);

    reader_ = std::make_unique<NativeStreamReader>();
    
    // Connect the raw packet callback to emit a signal for recording pipelines
    reader_->setPacketCallback([this](const uint8_t* data, int size, bool is_keyframe) {
        if (!should_stop_) {
            QByteArray packetData((const char*)data, size);
            Q_EMIT rawPacketReady(packetData, is_keyframe);
        }
    });
}

NativeReaderWorker::~NativeReaderWorker() {
    stop();
}

void NativeReaderWorker::stop() {
    should_stop_ = true;
    if (reader_) {
        // Step 1: Signal abort ONLY (sets flags, does NOT free resources).
        // The interrupt callback will cause av_read_frame() to return immediately.
        // The run() loop will see should_stop_/!isConnected() and exit.
        reader_->abort();
    }
    // Step 2: Wait for run() to fully exit.
    // After this returns, the processing loop is guaranteed to be done and no more
    // pool tasks will be submitted.
    if (QThread::currentThread() != this && isRunning()) {
        if (!wait(10000)) { // 10s timeout to prevent infinite hang
            LOG_WARN("[NativeWorker-{}] Thread did not stop in 10s, forcing termination", camera_id_);
            terminate();
            wait(2000);
        }
    }
    // Step 3: Wait for any in-flight processing task to finish.
    // Safe to access now — emit thread is already joined.
    if (processing_future_.isRunning()) {
        processing_future_.waitForFinished();
    }
    processing_pool_.waitForDone(3000);
    // Step 4: NOW it is safe to free FFmpeg resources.
    if (reader_) {
        reader_->close();
    }
}

void NativeReaderWorker::run() {
    LOG_INFO("[NativeWorker-{}] Starting unified reader thread for URL: {}", camera_id_, url_);

    // Reconnect loop — handles transient network glitches without restarting the pipeline.
    // Uses exponential backoff (1s→2s→4s→…→60s cap) to prevent restart storms.
    constexpr int MAX_RECONNECTS = 10;
    constexpr int BASE_DELAY_MS = 1000;
    constexpr int MAX_DELAY_MS = 60000;
    int reconnect_count = 0;

    auto backoffDelayMs = [](int attempt) -> int {
        // 1s * 2^attempt, capped at 60s
        long long delay = static_cast<long long>(BASE_DELAY_MS) * (1LL << std::min(attempt, 6));
        return static_cast<int>(std::min(delay, static_cast<long long>(MAX_DELAY_MS)));
    };

    LOG_INFO("[NativeWorker-{}] [STATE] CONNECTING", camera_id_);

    while (!should_stop_) {
        if (!reader_->open(url_)) {
            reconnect_count++;
            if (reconnect_count > MAX_RECONNECTS) {
                LOG_ERROR("[NativeWorker-{}] [STATE] FAILED — max reconnect attempts ({}) exhausted. "
                          "Operator action required.",
                          camera_id_, MAX_RECONNECTS);
                Q_EMIT streamFailed();
                break;
            }
            int delay_ms = backoffDelayMs(reconnect_count - 1);
            LOG_WARN("[NativeWorker-{}] [STATE] DEGRADED — open failed (attempt {}/{}), "
                     "retrying in {}ms",
                     camera_id_, reconnect_count, MAX_RECONNECTS, delay_ms);
            QThread::msleep(delay_ms);
            continue;
        }

        // Connected — reset reconnect counter and log state transition
        reconnect_count = 0;
        LOG_INFO("[NativeWorker-{}] [STATE] RUNNING", camera_id_);
        Q_EMIT streamConnected();

        // ── Direct processing loop (Synchronized to avoid stutter and save CPU) ──────────────
        auto last_emit = std::chrono::steady_clock::now();
        cv::Mat raw_frame;

        while (!should_stop_ && reader_->isConnected()) {
            auto now = std::chrono::steady_clock::now();
            // Read target_fps_ live so adaptive FPS changes take effect immediately.
            int fps = target_fps_.load(std::memory_order_relaxed);
            if (fps <= 0) fps = 15;
            int interval_ms = 1000 / fps;
            bool need_frame = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_emit).count() >= (interval_ms - 2);

            // Decode to 960×540 for display quality. SHM gets a separate 640×360 resize
            // (cv::resize ~0.5ms) — cheaper than the previous sws_ctx recreation bug (~30ms).
            // raw_frame is intentionally NOT reset between frames: keeping it allocated avoids
            // triggering sws_ctx recreation in readFrame (sws_getContext costs ~30ms per call).
            if (!reader_->readFrame(raw_frame, !need_frame, 960, 540)) {
                if (!should_stop_) {
                    LOG_WARN("[NativeWorker-{}] [NETWORK] Read frame failed", camera_id_);
                }
                break;
            }

            if (!need_frame) {
                // Sleep ~1ms to avoid busy-spinning while waiting for next frame interval
                QThread::msleep(1);
                continue;
            }

            if (need_frame && !raw_frame.empty()) {
                last_emit = std::chrono::steady_clock::now();

                // Drop frame if previous encode/SHM task is still running to prevent queue buildup
                if (processing_future_.isFinished() || !processing_future_.isRunning()) {
                    uint64_t fid = frame_count_++;
                    processAndEmit(raw_frame.clone(), fid);
                }
                // DO NOT reset raw_frame to cv::Mat{} — keeping it allocated (960×540)
                // prevents sws_ctx recreation in readFrame on the next need_frame call.
            }
        }

        // If stopping, break the outer reconnect loop
        if (should_stop_) break;

        // Stream lost — close resources and try to reconnect
        LOG_WARN("[NativeWorker-{}] [STATE] DEGRADED — stream lost", camera_id_);
        Q_EMIT streamDisconnected();
        reader_->close();

        reconnect_count++;
        if (reconnect_count > MAX_RECONNECTS) {
            LOG_ERROR("[NativeWorker-{}] [STATE] FAILED — max reconnect attempts ({}) exhausted. "
                      "Operator action required.",
                      camera_id_, MAX_RECONNECTS);
            Q_EMIT streamFailed();
            break;
        }
        int delay_ms = backoffDelayMs(reconnect_count - 1);
        LOG_WARN("[NativeWorker-{}] [NETWORK] Reconnecting in {}ms ({}/{})",
                 camera_id_, delay_ms, reconnect_count, MAX_RECONNECTS);
        QThread::msleep(delay_ms);
    }

    LOG_INFO("[NativeWorker-{}] Exiting thread", camera_id_);
    Q_EMIT streamDisconnected();
}

// ── Shared encode+emit logic (RTSP emit thread → processing_pool_) ────────────
void NativeReaderWorker::processAndEmit(cv::Mat small_frame, uint64_t fid) {
    processing_future_ = QtConcurrent::run(&processing_pool_, [this, small_frame = std::move(small_frame), fid, camera_id = camera_id_]() {
        // small_frame is 960×540 (display quality). ws_frame = same Mat for WebSocket emit.
        // shm_frame is resized to 640×360 (SHM_FRAME_SIZE = 640×360×3 = 691,200 bytes).
        const cv::Mat& ws_frame = small_frame;
        std::vector<unsigned char> jpeg_data;
        cv::imencode(".jpg", ws_frame, jpeg_data, {cv::IMWRITE_JPEG_QUALITY, 75});

        std::vector<inference::TrackedObject> current_objects;
        nlohmann::json meta;
        auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (shm_manager_ && shm_manager_->isInitialized()) {
            cv::Mat shm_frame;
            cv::resize(small_frame, shm_frame, cv::Size(640, 360), 0, 0, cv::INTER_NEAREST);
            shm_manager_->writeVideoFrame(shm_frame, fid, now_ms);

            // AI inference runs on shm_frame (640×360); snapshot crops use ws_frame (960×540).
            // Scale bbox coords so cropSnapshot and ROI polygon checks work in the same space.
            const float sx = (float)ws_frame.cols / (float)shm_frame.cols;
            const float sy = (float)ws_frame.rows / (float)shm_frame.rows;

            inference::MultiModelResult ai_result;
            const bool md_ok = shm_manager_->readMetadata(ai_result, current_objects);
            // Diagnostic: log AI presence every ~10s so user can verify the AI worker
            // subprocess is actually writing to SHM. If md_ok=false → SHM magic check
            // failing (AI worker not running / SHM not initialized). If md_ok=true but
            // counts always 0 → AI worker running but producing no detections (model load
            // failed, threshold too high, no targets in frame).
            {
                static thread_local uint64_t last_diag_ms = 0;
                if (now_ms - last_diag_ms > 10000) {
                    last_diag_ms = now_ms;
                    LOG_INFO("[AI-DIAG cam={}] readMetadata={}, objects={}, faces={}, plates={}",
                             camera_id, md_ok ? "ok" : "FAIL",
                             current_objects.size(), ai_result.faces.size(), ai_result.plates.size());
                }
            }
            if (md_ok) {
                meta["camera_id"] = camera_id;
                nlohmann::json objs = nlohmann::json::array();
                for (const auto& obj : current_objects) {
                    objs.push_back({
                        {"type", obj.label},
                        {"class_id", obj.bbox.class_id},
                        {"track_id", obj.track_id},
                        {"confidence", obj.confidence},
                        {"bbox", {(int)(obj.bbox.x1 * sx), (int)(obj.bbox.y1 * sy),
                                  (int)(obj.bbox.x2 * sx), (int)(obj.bbox.y2 * sy)}}
                    });
                }
                for (const auto& face : ai_result.faces) {
                    objs.push_back({
                        {"type", "Face"},
                        {"class_name", (face.person_id != -1 && !face.name.empty()) ? face.name : "Face"},
                        {"label",      (face.person_id != -1 && !face.name.empty()) ? face.name : "Face"},
                        {"class_id", 100},
                        {"track_id", face.person_id},
                        {"person_id", face.person_id},
                        {"confidence", face.confidence},
                        {"similarity", face.similarity},
                        {"bbox", {(int)(face.x1 * sx), (int)(face.y1 * sy),
                                  (int)(face.x2 * sx), (int)(face.y2 * sy)}}
                    });
                }
                for (const auto& plate : ai_result.plates) {
                    objs.push_back({
                        {"type", "Plate"},
                        {"class_id", 101},
                        {"label", plate.text},
                        {"confidence", plate.confidence},
                        {"bbox", {(int)(plate.x1 * sx), (int)(plate.y1 * sy),
                                  (int)(plate.x2 * sx), (int)(plate.y2 * sy)}}
                    });
                }
                meta["objects"] = objs;
            }
        }

        // Push JPEG to WebSocket clients directly from this worker thread — bypasses the
        // Qt::QueuedConnection hop to main thread (~15ms on Windows default timer resolution).
        // broadcastRawFrame is thread-safe: uses shared_mutex internally, and
        // sendBinaryDropIfBusy posts the actual socket write via invokeMethod to the socket thread.
        if (!jpeg_data.empty()) {
            // Pass 640×360 as bbox coordinate space (AI detection space, matches SHM).
            // JPEG is 960×540 but aspect ratio is identical (16:9) so canvas scaling is correct.
            vms::streaming::CameraStreamManager::getInstance().broadcastRawFrame(
                camera_id,
                *reinterpret_cast<const std::vector<uint8_t>*>(&jpeg_data),
                current_objects, 640, 360
            );
        }

        // PERF-OPT: AiEventProcessor only inspects metadata every 500ms. Cloning the
        // 960×540×3 = 1.5MB Mat on every emit (25fps = 37 MB/s/cam) caused a measurable
        // memory bandwidth tax that contributed to the live-stream stutter under load.
        //
        // We pass a real Mat ONLY on the ~2fps cadence the consumer actually uses, and
        // an empty Mat on the rest of the frames (handleFrameProcessed already gates the
        // expensive call by timestamp; passing empty just lets it bump fps counters).
        //
        // last_emit_full_frame_ms_ is per-worker; even if a viewer is hammering at 25fps,
        // ~2 full clones/sec is enough for snapshot crops and ROI polygon checks.
        const uint64_t MIN_FULL_FRAME_INTERVAL_MS = 500;
        bool emit_full_frame = (now_ms - last_emit_full_frame_ms_.load(std::memory_order_relaxed))
                                >= MIN_FULL_FRAME_INTERVAL_MS;
        if (emit_full_frame) {
            last_emit_full_frame_ms_.store(now_ms, std::memory_order_relaxed);
            Q_EMIT frameProcessed(camera_id, ws_frame.clone(), {}, current_objects, meta, now_ms);
        } else {
            // Lightweight emit: empty Mat so we still advance fps counter and update last_objects,
            // but skip the per-frame 1.5MB memcpy.
            Q_EMIT frameProcessed(camera_id, cv::Mat{}, {}, current_objects, meta, now_ms);
        }
    });
}

} // namespace core
} // namespace vms
