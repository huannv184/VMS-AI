#include "core/native_reader_worker.h"
#include "utils/logger.h"
#include <algorithm>
#include <chrono>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <QtConcurrent>
#include "ai/ipc/shared_memory_manager.h"
#include "inference/multi_model_infer.h"
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

    // One dedicated thread per camera: tasks run immediately, no queuing
    // behind other cameras in the global QThreadPool.
    processing_pool_.setMaxThreadCount(1);

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
    // Step 2: Wait for any background processing task to finish.
    if (processing_future_.isRunning()) {
        processing_future_.waitForFinished();
    }
    processing_pool_.waitForDone(3000);
    // Step 3: Wait for the thread to fully exit run().
    // After this returns, readFrame() is guaranteed to not be executing.
    if (QThread::currentThread() != this && isRunning()) {
        if (!wait(10000)) { // 10s timeout to prevent infinite hang
            LOG_WARN("[NativeWorker-{}] Thread did not stop in 10s, forcing termination", camera_id_);
            terminate();
            wait(2000);
        }
    }
    // Step 3: NOW it is safe to free FFmpeg resources (codec, frame, packet, etc.)
    // because the thread is fully stopped and no longer using them.
    if (reader_) {
        reader_->close();
    }
}

void NativeReaderWorker::run() {
    LOG_INFO("[NativeWorker-{}] Starting NativeLibavReader thread for URL: {}", camera_id_, url_);

    // Detect local webcam mode: exact match OR RTSP URL ending with /webcam or /0
    bool isWebcam = (url_ == "webcam" || url_ == "0");
    if (!isWebcam) {
        // Also match rtsp://...anything.../webcam (from frontend that prepended rtsp://)
        auto pos = url_.rfind('/');
        if (pos != std::string::npos) {
            std::string tail = url_.substr(pos + 1);
            if (tail == "webcam" || tail == "0") isWebcam = true;
        }
    }

    if (isWebcam) {
        cv::VideoCapture cap(0);
        if (!cap.isOpened()) {
            LOG_ERROR("[NativeWorker-{}] Failed to open local webcam", camera_id_);
            Q_EMIT streamDisconnected();
            return;
        }

        // Standard HD resolution for processing
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);

        Q_EMIT streamConnected();
        cv::Mat raw_frame;
        
        while (!should_stop_) {
            if (!cap.read(raw_frame)) {
                break;
            }
            if (raw_frame.empty()) continue;
            
            // --- OPTIMIZATION: Process frame on per-camera thread pool ---
            if (!processing_future_.isRunning()) {
                cv::Mat frame_clone = raw_frame.clone();
                uint64_t fid = frame_count_++;

                processing_future_ = QtConcurrent::run(&processing_pool_, [this, frame_clone, fid, camera_id = camera_id_]() {
                    cv::Mat small_frame = fastDownscale(frame_clone, 640, 360);

                    std::vector<unsigned char> jpeg_data;
                    cv::imencode(".jpg", small_frame, jpeg_data, {cv::IMWRITE_JPEG_QUALITY, 60});

                    std::vector<inference::TrackedObject> current_objects;
                    nlohmann::json meta;
                    auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    if (shm_manager_ && shm_manager_->isInitialized()) {
                        shm_manager_->writeVideoFrame(small_frame, fid, now_ms);
                        
                        inference::MultiModelResult ai_result;
                        if (shm_manager_->readMetadata(ai_result, current_objects)) {
                            meta["camera_id"] = camera_id;
                            nlohmann::json objs = nlohmann::json::array();
                            for (const auto& obj : current_objects) {
                                objs.push_back({
                                    {"type", obj.label},
                                    {"class_id", obj.bbox.class_id},
                                    {"track_id", obj.track_id},
                                    {"confidence", obj.confidence},
                                    {"bbox", {(int)obj.bbox.x1, (int)obj.bbox.y1, (int)obj.bbox.x2, (int)obj.bbox.y2}}
                                });
                            }
                            for (const auto& face : ai_result.faces) {
                                objs.push_back({
                                    {"type", "Face"},
                                    {"class_name", (face.person_id != -1 && !face.name.empty()) ? face.name : "Face"},
                                    {"label", (face.person_id != -1 && !face.name.empty()) ? face.name : "Face"},
                                    {"class_id", 100},
                                    {"track_id", face.person_id},
                                    {"person_id", face.person_id},
                                    {"confidence", face.confidence},
                                    {"similarity", face.similarity},
                                    {"bbox", {(int)face.x1, (int)face.y1, (int)face.x2, (int)face.y2}}
                                });
                            }
                            for (const auto& plate : ai_result.plates) {
                                objs.push_back({
                                    {"type", "Plate"},
                                    {"class_id", 101},
                                    {"label", plate.text},
                                    {"confidence", plate.confidence},
                                    {"bbox", {(int)plate.x1, (int)plate.y1, (int)plate.x2, (int)plate.y2}}
                                });
                            }
                            meta["objects"] = objs;
                        }
                    }

                    // Emit processed for Live Stream consumers (Main thread only forwards this)
                    Q_EMIT frameProcessed(camera_id, jpeg_data, current_objects, meta, now_ms);
                });
            }

            // Emit raw for AI consumers if needed
            Q_EMIT frameReady(raw_frame);
            
            // Adaptive rate: honour target_fps_ set by the watchdog.
            int fps = target_fps_.load(std::memory_order_relaxed);
            QThread::msleep(fps > 0 ? (1000 / fps) : 1000);
        }
        cap.release();
        Q_EMIT streamDisconnected();
        return;
    }

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

        cv::Mat raw_frame;
        auto last_emit = std::chrono::steady_clock::now();

        while (!should_stop_ && reader_->isConnected()) {
            if (!reader_->readFrame(raw_frame)) {
                if (!should_stop_) {
                    LOG_WARN("[NativeWorker-{}] [NETWORK] Read frame failed — stream lost or camera closed connection",
                             camera_id_);
                }
                break;
            }

            if (raw_frame.empty()) continue;

            auto now = std::chrono::steady_clock::now();
            // Recompute interval each frame so watchdog FPS changes take effect immediately.
            int fps = target_fps_.load(std::memory_order_relaxed);
            auto frame_interval = std::chrono::milliseconds(fps > 0 ? (1000 / fps) : 1000);
            if (now - last_emit >= frame_interval) {
                // FIX: Only advance last_emit when a task is actually submitted.
                // Previously last_emit was always advanced, even when the task
                // was still running and the frame was silently dropped. That
                // caused a systematic 2×frame_interval stutter: every dropped
                // frame pushed last_emit forward, delaying the next submit by
                // another full interval. Now, if the pool is busy we simply
                // retry on the next readFrame() iteration (≤ source-fps ms later).
                if (!processing_future_.isRunning()) {
                    cv::Mat small_frame = fastDownscale(raw_frame, 640, 360);
                    uint64_t fid = frame_count_++;

                    processing_future_ = QtConcurrent::run(&processing_pool_, [this, small_frame, fid, camera_id = camera_id_]() {
                        std::vector<unsigned char> jpeg_data;
                        cv::imencode(".jpg", small_frame, jpeg_data, {cv::IMWRITE_JPEG_QUALITY, 60});

                        std::vector<inference::TrackedObject> current_objects;
                        nlohmann::json meta;
                        auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();

                        if (shm_manager_ && shm_manager_->isInitialized()) {
                            shm_manager_->writeVideoFrame(small_frame, fid, now_ms);

                            inference::MultiModelResult ai_result_rtsp;
                            if (shm_manager_->readMetadata(ai_result_rtsp, current_objects)) {
                                meta["camera_id"] = camera_id;
                                nlohmann::json objs = nlohmann::json::array();
                                for (const auto& obj : current_objects) {
                                    objs.push_back({
                                        {"type", obj.label},
                                        {"class_id", obj.bbox.class_id},
                                        {"track_id", obj.track_id},
                                        {"confidence", obj.confidence},
                                        {"bbox", {(int)obj.bbox.x1, (int)obj.bbox.y1, (int)obj.bbox.x2, (int)obj.bbox.y2}}
                                    });
                                }
                                for (const auto& face : ai_result_rtsp.faces) {
                                    objs.push_back({
                                        {"type", "Face"},
                                        {"class_name", (face.person_id != -1 && !face.name.empty()) ? face.name : "Face"},
                                        {"label", (face.person_id != -1 && !face.name.empty()) ? face.name : "Face"},
                                        {"class_id", 100},
                                        {"track_id", face.person_id},
                                        {"person_id", face.person_id},
                                        {"confidence", face.confidence},
                                        {"similarity", face.similarity},
                                        {"bbox", {(int)face.x1, (int)face.y1, (int)face.x2, (int)face.y2}}
                                    });
                                }
                                for (const auto& plate : ai_result_rtsp.plates) {
                                    objs.push_back({
                                        {"type", "Plate"},
                                        {"class_id", 101},
                                        {"label", plate.text},
                                        {"confidence", plate.confidence},
                                        {"bbox", {(int)plate.x1, (int)plate.y1, (int)plate.x2, (int)plate.y2}}
                                    });
                                }
                                meta["objects"] = objs;
                            }
                        }

                        // Emit processed for Live Stream (Lean Main Thread)
                        Q_EMIT frameProcessed(camera_id, jpeg_data, current_objects, meta, now_ms);
                    });
                    last_emit = now; // advance only on actual submit
                }
                // if pool busy: don't advance last_emit — retry next readFrame iteration
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

} // namespace core
} // namespace vms
