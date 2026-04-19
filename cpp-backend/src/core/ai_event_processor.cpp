#include "core/ai_event_processor.h"
#include "core/event_manager.h"
#include "core/roi_manager.h"
#include "utils/logger.h"
#include "utils/storage_manager.h"
#include <filesystem>
#include <chrono>
#include <thread>
#include <opencv2/imgproc.hpp>

namespace vms {
namespace core {

AiEventProcessor::AiEventProcessor() {
    // Single background thread drains MinIO upload queue.
    // Decouples network I/O from the event-processing threads entirely.
    upload_worker_ = std::thread(&AiEventProcessor::uploadWorkerLoop, this);
}

AiEventProcessor::~AiEventProcessor() {
    {
        std::lock_guard<std::mutex> lk(upload_mutex_);
        upload_stop_ = true;
    }
    upload_cv_.notify_all();
    if (upload_worker_.joinable()) upload_worker_.join();
}

AiEventProcessor& AiEventProcessor::getInstance() {
    static AiEventProcessor instance;
    return instance;
}

// ── Upload worker ─────────────────────────────────────────────────────────────

void AiEventProcessor::uploadWorkerLoop() {
    while (true) {
        UploadTask task;
        {
            std::unique_lock<std::mutex> lk(upload_mutex_);
            upload_cv_.wait(lk, [this] { return !upload_queue_.empty() || upload_stop_; });
            if (upload_stop_ && upload_queue_.empty()) break;
            task = std::move(upload_queue_.front());
            upload_queue_.pop();
        }
        try {
            vms::utils::StorageManager::getInstance().uploadFile(task.local_path, task.object_key);
        } catch (const std::exception& e) {
            LOG_WARN("AiEventProcessor: upload failed for {}: {}", task.object_key, e.what());
        }
    }
}

void AiEventProcessor::enqueueUpload(std::string local_path, std::string object_key) {
    std::lock_guard<std::mutex> lk(upload_mutex_);
    if (static_cast<int>(upload_queue_.size()) >= MAX_UPLOAD_QUEUE) {
        LOG_WARN("AiEventProcessor: upload queue full — dropping snapshot {}", object_key);
        return;
    }
    upload_queue_.push({std::move(local_path), std::move(object_key)});
    upload_cv_.notify_one();
}

// ── Public entry point ────────────────────────────────────────────────────────

void AiEventProcessor::processMetadata(int camera_id, const nlohmann::json& metadata, const std::vector<uchar>& jpeg_data) {
    if (metadata.is_null() || !metadata.contains("objects") || metadata["objects"].empty() || jpeg_data.empty()) {
        return;
    }

    // Cap concurrent event threads. If all slots are busy, drop this batch
    // rather than spawning an unbounded number of threads.
    int prev = active_event_threads_.load(std::memory_order_acquire);
    if (prev >= MAX_EVENT_THREADS) {
        return; // system under load — skip this batch
    }
    if (!active_event_threads_.compare_exchange_strong(prev, prev + 1,
                                                       std::memory_order_acq_rel)) {
        return; // lost the race — another thread just filled a slot
    }

    // Copy by value for thread safety. jpeg_data copy is ~10-40 KB — acceptable.
    std::thread([this, camera_id, metadata, jpeg_data]() {
        try {
            // Decode JPEG once for all objects in this frame — was decoded N times before.
            cv::Mat frame = cv::imdecode(jpeg_data, cv::IMREAD_COLOR);

            for (const auto& obj : metadata["objects"]) {
                std::string type = obj.value("type", obj.value("class_name", ""));
                int track_id = obj.value("track_id", -1);
                int class_id = obj.value("class_id", -1);

                if (class_id == 100 || type == "Face" || type == "face") {
                    if (track_id != -1) {
                        processFace(camera_id, obj, frame);
                    }
                } else if (class_id == 0 || type == "person" || type == "Person") {
                    processIntrusion(camera_id, obj, frame);
                    processLineCrossing(camera_id, obj, frame);
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("AiEventProcessor: Failed to process metadata: {}", e.what());
        }
        active_event_threads_.fetch_sub(1, std::memory_order_release);
    }).detach();
}

// ── Per-event handlers ────────────────────────────────────────────────────────

void AiEventProcessor::processFace(int camera_id, const nlohmann::json& obj, const cv::Mat& frame) {
    int track_id = obj.value("track_id", -1);
    std::string key = std::to_string(camera_id) + ":" + std::to_string(track_id) + ":face";

    if (isOnCooldown(key)) return;

    int person_id = obj.value("person_id", track_id);
    double confidence = obj.value("confidence", 0.0);

    cv::Mat crop = cropSnapshot(frame, obj.value("bbox", nlohmann::json::array()));
    std::string snapshot_path = saveSnapshot(camera_id, crop);

    Event event;
    event.camera_id = camera_id;
    event.event_type = "FACE_RECOGNIZED";
    event.description = "Face detected with ID " + std::to_string(person_id);
    event.timestamp = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();

    nlohmann::json enhanced_meta = obj;
    enhanced_meta["person_id"] = person_id;
    enhanced_meta["confidence"] = confidence;
    enhanced_meta["snapshot_url"] = snapshot_path;
    event.metadata_json = enhanced_meta.dump();

    EventManager::getInstance().createEvent(event);
    setCooldown(key);
    LOG_INFO("Face event: camera={} person_id={} conf={:.2f}", camera_id, person_id, confidence);
}

void AiEventProcessor::processIntrusion(int camera_id, const nlohmann::json& obj, const cv::Mat& frame) {
    int track_id = obj.value("track_id", -1);
    std::string key = std::to_string(camera_id) + ":" + std::to_string(track_id) + ":person";

    if (isOnCooldown(key)) return;

    double confidence = obj.value("confidence", 0.0);
    if (confidence < 0.5) return;

    auto j_bbox = obj.value("bbox", nlohmann::json::array());

    // ROI Filter Logic
    if (j_bbox.is_array() && j_bbox.size() >= 4) {
        int x1 = j_bbox[0].get<int>();
        int x2 = j_bbox[2].get<int>();
        int y2 = j_bbox[3].get<int>();
        
        // Use bottom-center of the bounding box as the point to check
        cv::Point2f bottom_center(static_cast<float>(x1 + x2) / 2.0f, static_cast<float>(y2));
        
        auto rois = vms::core::ROIManager::getInstance().getCameraROIs(camera_id);
        bool inside_roi = false;
        bool has_enabled_polygon_roi = false;
        
        for (const auto& roi : rois) {
            if (!roi.enabled || (roi.roi_type != "polygon" && roi.roi_type != "intrusion")) continue;
            
            try {
                auto points_json = nlohmann::json::parse(roi.points_json);
                if (points_json.is_array() && points_json.size() > 2) {
                    has_enabled_polygon_roi = true;
                    std::vector<cv::Point2f> poly_pts;
                    for (const auto& pt : points_json) {
                        float px = pt.value("x", 0.0f);
                        float py = pt.value("y", 0.0f);
                        // Convert normalized coordinates (0..1) to actual pixel coordinates
                        if (px <= 1.0f && py <= 1.0f) {
                            px *= frame.cols;
                            py *= frame.rows;
                        }
                        poly_pts.push_back(cv::Point2f(px, py));
                    }
                    
                    if (cv::pointPolygonTest(poly_pts, bottom_center, false) >= 0) {
                        inside_roi = true;
                        break;
                    }
                }
            } catch (...) {
                // Ignore parse errors for single ROI
            }
        }
        
        // If there are enabled polygon ROIs and the person is not inside any, do not trigger
        if (has_enabled_polygon_roi && !inside_roi) {
            return;
        }
    }

    cv::Mat crop = cropSnapshot(frame, j_bbox);
    std::string snapshot_path = saveSnapshot(camera_id, crop);

    Event event;
    event.camera_id = camera_id;
    event.event_type = "PERSON_DETECTED";
    event.description = "Person detected";
    event.timestamp = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();

    nlohmann::json enhanced_meta = obj;
    enhanced_meta["snapshot_url"] = snapshot_path;
    event.metadata_json = enhanced_meta.dump();

    EventManager::getInstance().createEvent(event);
    setCooldown(key);
    LOG_INFO("Person event: camera={} track={} conf={:.2f}", camera_id, track_id, confidence);
}

void AiEventProcessor::processLineCrossing(int camera_id, const nlohmann::json& obj, const cv::Mat& frame) {
    // Reserved for future implementation
}

// ── Helpers ───────────────────────────────────────────────────────────────────

bool AiEventProcessor::isOnCooldown(const std::string& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cooldown_cache_.find(key);
    if (it != cooldown_cache_.end()) {
        auto diff = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now() - it->second).count();
        if (diff < COOLDOWN_SECONDS) return true;
    }
    return false;
}

void AiEventProcessor::setCooldown(const std::string& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    // Evict oldest entries if cache exceeds limit to prevent unbounded growth.
    if (static_cast<int>(cooldown_cache_.size()) >= MAX_COOLDOWN_CACHE) {
        auto oldest = cooldown_cache_.begin();
        for (auto it = cooldown_cache_.begin(); it != cooldown_cache_.end(); ++it) {
            if (it->second < oldest->second) oldest = it;
        }
        cooldown_cache_.erase(oldest);
    }
    cooldown_cache_[key] = std::chrono::system_clock::now();
}

cv::Mat AiEventProcessor::cropSnapshot(const cv::Mat& frame, const nlohmann::json& bbox) {
    if (frame.empty() || !bbox.is_array() || bbox.size() < 4) return frame;
    try {
        int x1 = bbox[0].get<int>(), y1 = bbox[1].get<int>();
        int x2 = bbox[2].get<int>(), y2 = bbox[3].get<int>();
        constexpr int pad = 20;
        x1 = std::max(0, x1 - pad);
        y1 = std::max(0, y1 - pad);
        x2 = std::min(frame.cols, x2 + pad);
        y2 = std::min(frame.rows, y2 + pad);
        if (x2 > x1 && y2 > y1) return frame(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
    } catch (...) {}
    return frame;
}

std::string AiEventProcessor::saveSnapshot(int camera_id, const cv::Mat& crop) {
    if (crop.empty()) return "";

    std::string base_dir = "data/snapshots";
    if (!std::filesystem::exists(base_dir)) {
        std::filesystem::create_directories(base_dir);
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string filename = "cam" + std::to_string(camera_id) + "_" + std::to_string(ms) + ".jpg";
    std::string full_path = base_dir + "/" + filename;
    std::string object_key = "snapshots/" + filename;

    cv::imwrite(full_path, crop, {cv::IMWRITE_JPEG_QUALITY, 85});

    // Non-blocking: enqueue MinIO upload to dedicated worker thread.
    enqueueUpload(full_path, object_key);

    return object_key;
}

} // namespace core
} // namespace vms
