#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <queue>
#include <vector>
#include <functional>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>

namespace vms {
namespace core {

class AiEventProcessor {
public:
    static AiEventProcessor& getInstance();

    AiEventProcessor();
    ~AiEventProcessor();

    void processMetadata(int camera_id, const nlohmann::json& metadata, const cv::Mat& frame);

private:
    // ── Event processing: bounded worker pool with joinable workers ───────────
    // Previously processMetadata() spawned std::thread(...).detach() with `this`
    // captured. Detached workers held a pointer to the singleton without any
    // synchronisation against destruction — at process shutdown the destructor
    // would join only the upload worker and return, while detached workers were
    // still mid-flight reading cooldown_cache_/upload_mutex_/etc. That's a
    // textbook race / use-after-mutex-destruction. Replaced with N workers that
    // drain a bounded queue and are joined in the destructor under event_stop_.
    static constexpr int MAX_EVENT_THREADS = 2;
    static constexpr int MAX_EVENT_QUEUE = 16; // bounded backpressure: drop overflow
    struct EventJob {
        int camera_id{0};
        nlohmann::json metadata;
        cv::Mat frame;       // owned clone — safe across thread boundary
        int64_t ts_ms{0};
    };
    std::queue<EventJob> event_queue_;
    std::mutex event_queue_mutex_;
    std::condition_variable event_queue_cv_;
    std::vector<std::thread> event_workers_;
    bool event_stop_{false};
    void eventWorkerLoop();

    // ── Async upload queue (single worker thread, bounded to 32 tasks) ────────
    static constexpr int MAX_UPLOAD_QUEUE = 32;
    struct UploadTask { std::string local_path; std::string object_key; };
    std::queue<UploadTask> upload_queue_;
    std::mutex upload_mutex_;
    std::condition_variable upload_cv_;
    std::thread upload_worker_;
    bool upload_stop_{false};
    void uploadWorkerLoop();
    void enqueueUpload(std::string local_path, std::string object_key);

    // ── Detection cooldown ────────────────────────────────────────────────────
    std::unordered_map<std::string, std::chrono::system_clock::time_point> cooldown_cache_;
    std::mutex cache_mutex_;
    static constexpr int COOLDOWN_SECONDS = 10;
    static constexpr int MAX_COOLDOWN_CACHE = 1000;

    // Accepts pre-decoded frame to avoid repeated imdecode per object
    void processFace(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    void processIntrusion(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    void processLicensePlate(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    // BUG-REID-DEAD-PIPELINE producer: cross-camera ReID enrichment for every
    // person detection with a stable track_id. Engine handles its own cache
    // (track_to_global_) so per-frame calls short-circuit cheaply on known
    // tracks; only the first observation of a (camera, track) pair runs the
    // DNN embedding extraction.
    void processReID(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    // Batched line-crossing pass — runs once per metadata batch, after the
    // per-object loop has populated the TrackerStateManager. Builds the
    // detection list internally so the tracker only advances once per frame.
    void processLineCrossings(int camera_id,
                              const nlohmann::json& metadata,
                              const cv::Mat& frame,
                              int64_t ts_ms);
    bool isOnCooldown(const std::string& key);
    void setCooldown(const std::string& key);
    cv::Mat cropSnapshot(const cv::Mat& frame, const nlohmann::json& bbox);
    std::string saveSnapshot(int camera_id, const cv::Mat& crop);
};

} // namespace core
} // namespace vms
