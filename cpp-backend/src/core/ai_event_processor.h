#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <queue>
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

    void processMetadata(int camera_id, const nlohmann::json& metadata, const std::vector<uchar>& jpeg_data);

private:
    // ── Event processing: bounded thread pool (max 2 concurrent) ──────────────
    static constexpr int MAX_EVENT_THREADS = 2;
    std::atomic<int> active_event_threads_{0};

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
    void processLineCrossing(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    bool isOnCooldown(const std::string& key);
    void setCooldown(const std::string& key);
    cv::Mat cropSnapshot(const cv::Mat& frame, const nlohmann::json& bbox);
    std::string saveSnapshot(int camera_id, const cv::Mat& crop);
};

} // namespace core
} // namespace vms
