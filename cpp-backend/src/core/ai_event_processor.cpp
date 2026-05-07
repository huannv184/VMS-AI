#include "core/ai_event_processor.h"
#include "core/event_manager.h"
#include <QByteArray>
#include "core/attendance_tracker.h"
#include "core/camera_pipeline_manager.h"
#include "core/people_count_tracker.h"
#include "core/roi_manager.h"
#include "core/tracker_state_manager.h"
#include "events/rule_engine.h"
#include "events/event_types.h"
#include "utils/logger.h"
#include "utils/storage_manager.h"
#include <filesystem>
#include <chrono>
#include <thread>
#include <opencv2/imgproc.hpp>

namespace vms {
namespace core {

AiEventProcessor::AiEventProcessor() {
    // Bounded worker pool drains the per-frame metadata queue. Workers are
    // joined in the destructor so the singleton's mutexes and caches stay
    // alive for the entire lifetime of any in-flight job.
    event_workers_.reserve(MAX_EVENT_THREADS);
    for (int i = 0; i < MAX_EVENT_THREADS; ++i) {
        event_workers_.emplace_back(&AiEventProcessor::eventWorkerLoop, this);
    }
    // Single background thread drains MinIO upload queue.
    // Decouples network I/O from the event-processing threads entirely.
    upload_worker_ = std::thread(&AiEventProcessor::uploadWorkerLoop, this);
}

AiEventProcessor::~AiEventProcessor() {
    // Stop event workers first — they enqueue uploads, so they must drain
    // before the upload worker is asked to stop, otherwise the last few
    // snapshots would be silently dropped.
    {
        std::lock_guard<std::mutex> lk(event_queue_mutex_);
        event_stop_ = true;
    }
    event_queue_cv_.notify_all();
    for (auto& t : event_workers_) {
        if (t.joinable()) t.join();
    }
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

void AiEventProcessor::processMetadata(int camera_id, const nlohmann::json& metadata, const cv::Mat& frame) {
    if (metadata.is_null() || !metadata.contains("objects") || metadata["objects"].empty() || frame.empty()) {
        return;
    }

    // Resolve the frame timestamp once. Worker emits ms; if missing fall back
    // to wall-clock so TrackerStateManager's GC still ticks.
    int64_t ts_ms = metadata.value("timestamp_ms",
                       metadata.value("timestamp",
                           static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count())));

    EventJob job;
    job.camera_id = camera_id;
    job.metadata  = metadata;
    job.frame     = frame.clone(); // workers own their pixels independently of the pipeline cache
    job.ts_ms     = ts_ms;

    {
        std::lock_guard<std::mutex> lk(event_queue_mutex_);
        if (event_stop_) return; // shutting down — drop
        if (event_queue_.size() >= static_cast<size_t>(MAX_EVENT_QUEUE)) {
            // Bounded queue: drop oldest under load so newer state isn't starved
            // by stale frames piling up. Same effective drop policy as the prior
            // detached-thread cap, but without unbounded thread spawn.
            event_queue_.pop();
        }
        event_queue_.push(std::move(job));
    }
    event_queue_cv_.notify_one();
}

void AiEventProcessor::eventWorkerLoop() {
    while (true) {
        EventJob job;
        {
            std::unique_lock<std::mutex> lk(event_queue_mutex_);
            event_queue_cv_.wait(lk, [this] { return !event_queue_.empty() || event_stop_; });
            if (event_stop_ && event_queue_.empty()) break;
            job = std::move(event_queue_.front());
            event_queue_.pop();
        }
        try {
            for (const auto& obj : job.metadata["objects"]) {
                std::string type = obj.value("type", obj.value("class_name", ""));
                int class_id = obj.value("class_id", -1);

                if (class_id == 100 || type == "Face" || type == "face") {
                    processFace(job.camera_id, obj, job.frame);
                } else if (class_id == 0 || type == "person" || type == "Person") {
                    processIntrusion(job.camera_id, obj, job.frame);
                }
            }
            // Single tracker advance + line crossing pass per frame so the
            // greedy IoU matching sees all person detections together.
            processLineCrossings(job.camera_id, job.metadata, job.frame, job.ts_ms);
        } catch (const std::exception& e) {
            LOG_ERROR("AiEventProcessor: Failed to process metadata: {}", e.what());
        }
    }
}

// ── Per-event handlers ────────────────────────────────────────────────────────

void AiEventProcessor::processFace(int camera_id, const nlohmann::json& obj, const cv::Mat& frame) {
    int track_id = obj.value("track_id", -1);
    std::string key;
    if (track_id != -1) {
        key = std::to_string(camera_id) + ":" + std::to_string(track_id) + ":face";
    } else {
        // Position-based cooldown key for unrecognized faces (50px grid quantization)
        auto j_bbox = obj.value("bbox", nlohmann::json::array());
        int cx = 0, cy = 0;
        if (j_bbox.is_array() && j_bbox.size() >= 4) {
            cx = (j_bbox[0].get<int>() + j_bbox[2].get<int>()) / 2;
            cy = (j_bbox[1].get<int>() + j_bbox[3].get<int>()) / 2;
        }
        key = std::to_string(camera_id) + ":face:" + std::to_string(cx / 50) + ":" + std::to_string(cy / 50);
    }

    if (isOnCooldown(key)) return;

    int person_id = obj.value("person_id", track_id);
    double confidence = obj.value("confidence", 0.0);

    cv::Mat crop = cropSnapshot(frame, obj.value("bbox", nlohmann::json::array()));
    std::string snapshot_path = saveSnapshot(camera_id, crop);

    Event event;
    event.id = EventManager::generateEventId();
    event.camera_id = camera_id;
    event.event_type = "FACE_RECOGNIZED";
    event.description = "Face detected with ID " + std::to_string(person_id);
    event.snapshot_path = snapshot_path;
    event.timestamp = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();

    nlohmann::json enhanced_meta = obj;
    enhanced_meta["person_id"] = person_id;
    enhanced_meta["confidence"] = confidence;
    enhanced_meta["snapshot_url"] = snapshot_path;
    event.metadata_json = enhanced_meta.dump();

    // EventManager::createEvent triggers RuleEngine::evaluateEvent internally
    // (event_manager.cpp). Calling it again here would double every action:
    // 2× snapshots, 2× clip recordings, 2× webhooks, 2× alert broadcasts.
    EventManager::getInstance().createEvent(event);
    CameraPipelineManager::getInstance().triggerEventRecording(camera_id, event.id, 15, 5);
    setCooldown(key);

    // AttendanceTracker only counts true recognitions (person_id from face DB > 0);
    // Unknown faces (obj["person_id"] missing → -1) are dropped inside onFaceRecognized.
    int recognized_pid = obj.value("person_id", -1);
    if (recognized_pid > 0) {
        AttendanceTracker::getInstance().onFaceRecognized(
            camera_id, recognized_pid, static_cast<float>(confidence),
            event.timestamp, snapshot_path);
    }

    LOG_INFO("Face event: camera={} person_id={} conf={:.2f}", camera_id, person_id, confidence);
}

void AiEventProcessor::processIntrusion(int camera_id, const nlohmann::json& obj, const cv::Mat& frame) {
    int track_id = obj.value("track_id", -1);
    auto j_bbox = obj.value("bbox", nlohmann::json::array());

    // Position-based cooldown when tracker is bypassed (track_id=-1) so that
    // multiple persons in the same frame don't all share key `cam:-1:person`
    // — the first person would otherwise lock out everyone for COOLDOWN_SECONDS.
    // 50px grid is coarse enough for the same person stepping forward to keep
    // the same key, fine enough to separate distinct persons in the frame.
    std::string key;
    if (track_id != -1) {
        key = std::to_string(camera_id) + ":" + std::to_string(track_id) + ":person";
    } else {
        int cx = 0, cy = 0;
        if (j_bbox.is_array() && j_bbox.size() >= 4) {
            cx = (j_bbox[0].get<int>() + j_bbox[2].get<int>()) / 2;
            cy = (j_bbox[1].get<int>() + j_bbox[3].get<int>()) / 2;
        }
        key = std::to_string(camera_id) + ":person:" + std::to_string(cx / 50) + ":" + std::to_string(cy / 50);
    }

    if (isOnCooldown(key)) return;

    double confidence = obj.value("confidence", 0.0);
    if (confidence < 0.5) return;

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
    event.id = EventManager::generateEventId();
    event.camera_id = camera_id;
    event.event_type = "INTRUSION";
    event.description = "Person detected in monitored area";
    event.snapshot_path = snapshot_path;
    event.timestamp = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();

    nlohmann::json enhanced_meta = obj;
    enhanced_meta["snapshot_url"] = snapshot_path;
    event.metadata_json = enhanced_meta.dump();

    // EventManager::createEvent triggers RuleEngine internally — see processFace.
    EventManager::getInstance().createEvent(event);
    CameraPipelineManager::getInstance().triggerEventRecording(camera_id, event.id, 20, 5);
    setCooldown(key);

    LOG_INFO("Person event: camera={} track={} conf={:.2f}", camera_id, track_id, confidence);
}

void AiEventProcessor::processLineCrossings(int camera_id,
                                            const nlohmann::json& metadata,
                                            const cv::Mat& frame,
                                            int64_t ts_ms) {
    if (frame.empty() || !metadata.contains("objects")) return;

    // Build DetectionInputs for person-class objects only. Face objects
    // (class_id=100) carry no useful trajectory for occupancy counting.
    std::vector<DetectionInput> dets;
    dets.reserve(metadata["objects"].size());
    for (const auto& obj : metadata["objects"]) {
        const int class_id = obj.value("class_id", -1);
        const std::string type = obj.value("type", obj.value("class_name", std::string{}));
        if (class_id == 0 || type == "person" || type == "Person") {
            dets.push_back(DetectionInput::fromJson(obj));
        }
    }
    if (dets.empty()) return;

    // Advance the per-camera tracker (greedy IoU). Tracker is thread-safe;
    // multiple ai_event threads queued for the same camera serialize on the
    // per-camera mutex inside the manager.
    auto tracks = TrackerStateManager::getInstance().updateFrame(camera_id, dets, ts_ms);
    if (tracks.empty()) return;

    auto crossings = PeopleCountTracker::getInstance().checkCrossings(
        camera_id, tracks, frame.cols, frame.rows, ts_ms);
    if (crossings.empty()) return;

    for (const auto& c : crossings) {
        // Cooldown is already enforced inside PeopleCountTracker, so no
        // extra cooldown_cache_ check here — that map is for face/intrusion.
        cv::Rect roi(static_cast<int>(c.bbox.x),
                     static_cast<int>(c.bbox.y),
                     static_cast<int>(c.bbox.width),
                     static_cast<int>(c.bbox.height));
        roi &= cv::Rect(0, 0, frame.cols, frame.rows);
        cv::Mat crop = (roi.area() > 0) ? frame(roi).clone() : frame;
        std::string snapshot_path = saveSnapshot(camera_id, crop);

        Event event;
        event.id          = EventManager::generateEventId();
        event.camera_id   = camera_id;
        event.event_type  = (c.direction_code == "a_to_b") ? "LINE_CROSSING_A_TO_B"
                                                            : "LINE_CROSSING_B_TO_A";
        event.description = "Line '" + c.line_name + "' crossed (" + c.direction_label + ")";
        event.snapshot_path = snapshot_path;
        event.timestamp = c.ts_ms / 1000;

        nlohmann::json meta = {
            {"line_id",          c.line_id},
            {"line_name",        c.line_name},
            {"direction_code",   c.direction_code},
            {"direction_label",  c.direction_label},
            {"virtual_track_id", c.virtual_track_id},
            {"person_id",        c.person_id},
            {"object_class",     c.object_class},
            {"snapshot_url",     snapshot_path},
            {"bbox", {c.bbox.x, c.bbox.y, c.bbox.x + c.bbox.width, c.bbox.y + c.bbox.height}}
        };
        event.metadata_json = meta.dump();

        EventManager::getInstance().createEvent(event);

        LOG_INFO("Line crossing: camera={} line='{}' dir={} vid={} label='{}'",
                 camera_id, c.line_name, c.direction_code, c.virtual_track_id, c.direction_label);
    }
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
