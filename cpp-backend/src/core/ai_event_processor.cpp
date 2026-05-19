#include "core/ai_event_processor.h"
#include "core/event_manager.h"
#include <QByteArray>
#include "core/attendance_tracker.h"
#include "core/camera_pipeline_manager.h"
#include "core/people_count_tracker.h"
#include "core/roi_manager.h"
#include "core/tracker_state_manager.h"
#include "core/reid_engine.h"
#include "database/anpr_repository.h"
#include "events/rule_engine.h"
#include "events/event_types.h"
#include "streaming/camera_stream_manager_qt.h"
#include "utils/logger.h"
#include "utils/storage_manager.h"
#include <algorithm>
#include <cctype>
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

                // Worker writes either `label` (since 2026-04) or `type`/`class_name`
                // (legacy). Plate path keys on either label="LicensePlate" or
                // class_id=200 (see ai_worker/main.cpp:1047-1064).
                std::string label = obj.value("label", std::string(""));

                if (class_id == 100 || type == "Face" || type == "face") {
                    processFace(job.camera_id, obj, job.frame);
                } else if (class_id == 0 || type == "person" || type == "Person") {
                    // Two consumers per person detection:
                    //   1. processIntrusion — cooldown-gated event creation
                    //      (snapshot + ROI check + Event row).
                    //   2. processReID — cross-camera identity bookkeeping.
                    //      Engine's track_to_global_ cache means per-frame
                    //      calls on existing tracks are O(1); only the first
                    //      observation runs DNN embedding.
                    // Order: ReID first so a successful match can later be
                    // joined onto the intrusion event metadata (currently
                    // post-hoc via /api/reid/trail; future enrichment can
                    // attach global_id inline).
                    processReID(job.camera_id, obj, job.frame);
                    processIntrusion(job.camera_id, obj, job.frame);
                } else if (class_id == 200 || label == "LicensePlate") {
                    processLicensePlate(job.camera_id, obj, job.frame);
                } else if (class_id == 302 || class_id == 304 ||
                           label == "NoHelmet" || label == "NoVest") {
                    // 2026-05-19 PPE violation classes from PPE-YOLOv8 engine
                    // (post 300-offset remap in ai_worker): 302=NoHelmet,
                    // 304=NoVest. Engine ships 6 classes; ids 0-5 → 300-305.
                    processPPEViolation(job.camera_id, obj, job.frame);
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
    // ReID stitch: dispatch order in eventWorkerLoop runs processReID before
    // this handler, so by the time we reach here the engine's track_to_global_
    // cache has the gid for this (camera, track). Look it up cheaply (no DNN)
    // and embed in the event metadata so EventsView can show "Person #N" and
    // a "view trail" deep-link to /api/reid/trail/<gid>. -1 means ReID is
    // disabled / model missing / track_id was -1 — we omit the key rather
    // than serialize the sentinel.
    int reid_gid = ReIDEngine::getInstance().lookupGlobalId(camera_id, track_id);
    if (reid_gid > 0) {
        enhanced_meta["reid_global_id"] = reid_gid;
    }
    event.metadata_json = enhanced_meta.dump();

    // EventManager::createEvent triggers RuleEngine internally — see processFace.
    EventManager::getInstance().createEvent(event);
    CameraPipelineManager::getInstance().triggerEventRecording(camera_id, event.id, 20, 5);
    setCooldown(key);

    LOG_INFO("Person event: camera={} track={} conf={:.2f}", camera_id, track_id, confidence);
}

// ── PPE violation handler ────────────────────────────────────────────────────
//
// 2026-05-19 BUG-PPE-PHANTOM-FEATURE fix: ai_worker_v2 now feeds PPE-YOLOv8
// detections through the metadata.objects channel with class_ids 300-308
// (300-offset over PPE engine's 0-8 to avoid COCO class collision).
// Classes 303 (NO_SAFETY_VEST) and 304 (NO_HARD_HAT) are the violation
// classes; this handler emits one PPE_VIOLATION Event row per cooldown
// window per camera+location, matching the shape already consumed by
// PpeMonitorView via getEvents({event_type: 'PPE_VIOLATION'}).
//
// Cooldown is by bbox center bucket (50px grid) per camera + violation
// kind, NOT by track_id — PPE detections have track_id=-1 today (the
// engine outputs raw bboxes; tracker_state_manager only ingests COCO
// person class). 50 px bucket lets the same person re-trigger if they
// walk meaningfully across frame but suppresses static re-detection.
//
// Confidence floor 0.50: PPE conf_threshold in MultiModelInfer::Config
// is 0.45 (model-side NMS) — this 0.50 is the policy-side filter so an
// operator's PpeMonitorView never sees borderline detections that the
// model itself flagged with low confidence.
void AiEventProcessor::processPPEViolation(int camera_id,
                                          const nlohmann::json& obj,
                                          const cv::Mat& frame) {
    int class_id = obj.value("class_id", -1);
    std::string label = obj.value("label", std::string(""));
    double confidence = obj.value("confidence", 0.0);
    if (confidence < 0.50) return;

    // Build a stable kind tag for the cooldown key + event description.
    std::string kind;
    std::string desc;
    if (class_id == 302 || label == "NoHelmet") {
        kind = "no_helmet";
        desc = "Phát hiện nhân viên không đội mũ bảo hiểm";
    } else if (class_id == 304 || label == "NoVest") {
        kind = "no_vest";
        desc = "Phát hiện nhân viên không mặc áo phản quang";
    } else {
        return; // not a violation class
    }

    auto j_bbox = obj.value("box",
                            obj.value("bbox", nlohmann::json::array()));

    // Position bucket for cooldown (same idea as processIntrusion fallback).
    int cx = 0, cy = 0;
    if (j_bbox.is_array() && j_bbox.size() >= 4) {
        cx = (j_bbox[0].get<int>() + j_bbox[2].get<int>()) / 2;
        cy = (j_bbox[1].get<int>() + j_bbox[3].get<int>()) / 2;
    }
    std::string key = std::to_string(camera_id) + ":" + kind +
                      ":" + std::to_string(cx / 50) +
                      ":" + std::to_string(cy / 50);
    if (isOnCooldown(key)) return;

    cv::Mat crop = cropSnapshot(frame, j_bbox);
    std::string snapshot_path = saveSnapshot(camera_id, crop);

    Event event;
    event.id = EventManager::generateEventId();
    event.camera_id = camera_id;
    event.event_type = "PPE_VIOLATION";
    event.description = desc;
    event.snapshot_path = snapshot_path;
    event.timestamp = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();

    nlohmann::json enhanced_meta = obj;
    enhanced_meta["snapshot_url"] = snapshot_path;
    enhanced_meta["violation_kind"] = kind;   // "no_vest" | "no_hat"
    event.metadata_json = enhanced_meta.dump();

    EventManager::getInstance().createEvent(event);
    setCooldown(key);

    LOG_INFO("PPE violation: camera={} kind={} conf={:.2f} at ({},{})",
             camera_id, kind, confidence, cx, cy);
}

// ── License plate handler ────────────────────────────────────────────────────
//
// BUG-ANPR-PIPELINE half-dead fix (documented in synopsis_anpr_audit memory
// 2026-05-08b): ai_worker detects plates (multi_model_infer + LPR engine) and
// emits each plate as an object with class_id=200 / label="LicensePlate" /
// text="<plate string>" in the per-frame metadata. Pre-fix nothing on the
// backend side consumed that — the eventWorkerLoop switch only dispatched
// class_id 0/100 (person/face), so ANPRRepository::insertPlate had ZERO
// callers, the `license_plates` table stayed empty, and AnprView's gallery
// always rendered "no plates yet" no matter how many vehicles passed.
//
// Now: parallel to processIntrusion / processFace. Cooldown by camera + plate
// text (so the same plate sitting in frame for 30s doesn't insert 30 rows)
// rather than by track_id (worker writes track_id=-1 for plates). Empty-OCR
// detections are dropped — there's no point storing a row whose plate_number
// is "".
void AiEventProcessor::processLicensePlate(int camera_id,
                                           const nlohmann::json& obj,
                                           const cv::Mat& frame) {
    // OCR text is the only field worth keying on. Worker sets it from the
    // LPR OCR pass; missing/empty text means the detector framed a plate-shaped
    // region but OCR failed — useless for the gallery and would create rows
    // we can't index by plate_number. Drop silently.
    std::string plate_text = obj.value("text", std::string(""));
    // Trim whitespace + reject obvious garbage (length < 2). Conservative.
    plate_text.erase(plate_text.begin(),
                     std::find_if(plate_text.begin(), plate_text.end(),
                                  [](unsigned char ch) { return !std::isspace(ch); }));
    plate_text.erase(std::find_if(plate_text.rbegin(), plate_text.rend(),
                                  [](unsigned char ch) { return !std::isspace(ch); }).base(),
                     plate_text.end());
    if (plate_text.size() < 2) {
        return;
    }

    double confidence = obj.value("confidence", 0.0);
    // YOLO bbox conf is the only number worker emits per plate today. The OCR
    // pass would supply its own confidence in a richer message; until that's
    // wired, gate on the detection conf to suppress ghost detections.
    if (confidence < 0.40) {
        return;
    }

    // Cooldown key = camera + plate_text. Reading the same plate twice in <10s
    // is almost certainly the same vehicle in the same frame sequence; the
    // gallery doesn't gain anything from duplicate rows. Track_id is -1 for
    // plates today so we can't use it.
    std::string key = std::to_string(camera_id) + ":LPR:" + plate_text;
    if (isOnCooldown(key)) return;

    auto j_bbox = obj.value("box",
                            obj.value("bbox", nlohmann::json::array()));
    cv::Mat crop = cropSnapshot(frame, j_bbox);
    std::string snapshot_path = saveSnapshot(camera_id, crop);

    vms::LicensePlate plate;
    plate.plate_number = plate_text;
    plate.vehicle_type = obj.value("vehicle_type", std::string("unknown"));
    plate.color        = obj.value("color",        std::string(""));
    plate.camera_id    = camera_id;
    plate.confidence   = static_cast<float>(confidence);
    plate.image_path   = snapshot_path;
    plate.detected_at  = std::chrono::time_point_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now()).time_since_epoch().count();

    try {
        vms::database::ANPRRepository repo;
        if (!repo.insertPlate(plate)) {
            LOG_ERROR("ANPR insert failed: camera={} plate='{}'", camera_id, plate_text);
            return;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("ANPR insert exception: camera={} plate='{}' err={}",
                  camera_id, plate_text, e.what());
        return;
    }
    setCooldown(key);

    // Broadcast for the AnprView gallery to update in real-time without
    // waiting for the next poll cycle. Schema matches the generic alert
    // shape so useWebSocket.js handlers can route on event_type.
    nlohmann::json broadcast_msg = {
        {"type", "alert"},
        {"event_type", "LPR"},
        {"camera_id", camera_id},
        {"timestamp", plate.detected_at},
        {"severity", "low"},
        {"plate", plate_text},
        {"confidence", confidence},
        {"snapshot_url", snapshot_path}
    };
    try {
        auto& ws = vms::streaming::CameraStreamManager::getInstance();
        if (camera_id > 0) ws.broadcastEvent(camera_id, broadcast_msg);
        ws.broadcastEvent(0, broadcast_msg);
    } catch (...) {
        // WS broadcast failures must never break the DB-insert success path.
    }

    LOG_INFO("ANPR plate persisted: camera={} plate='{}' conf={:.2f}",
             camera_id, plate_text, confidence);
}

// ── Cross-camera ReID producer ────────────────────────────────────────────────
//
// BUG-REID-DEAD-PIPELINE producer fix (Face/ReID Audit 2026-05-08). Pre-fix:
//   - ReIDEngine::init() was never called from boot → initialized_ = false
//     forever → processDetection short-circuited and returned -1.
//   - Even if init had been called, no producer ever invoked processDetection
//     → gallery/trail/search REST endpoints returned [] forever, indistinguishable
//     from "scene quiet" (operators thought ReID worked but saw nothing).
//
// Producer side fix (here): every person detection (class_id=0) with a stable
// track_id is forwarded to ReIDEngine::processDetection. Engine maintains its
// own (camera_id, track_id) → global_id cache, so per-frame calls on already-
// seen tracks short-circuit in O(1) without rerunning the DNN. Only the first
// observation of a (cam, track) pair runs embedding extraction; subsequent
// frames just refresh last_seen on the existing entry.
//
// Why skip track_id=-1: ReID cross-camera matching needs a stable per-camera
// identity to detect "same track moving" vs "new person walked in". When the
// tracker is bypassed (Fix-B detect tuning default OFF, but operators can flip
// to bypass mode for high-FPS scenes), every detection is independent and ReID
// would create one global_id per frame — useless noise. Drop silently in that
// mode.
//
// Cost note: the engine's first-observation path runs a DNN forward (~10-50ms
// depending on model) inside the AiEventProcessor worker pool (MAX_EVENT_THREADS
// = 2). Sustained burst of NEW tracks across many cameras could backpressure;
// the bounded event_queue_ (MAX_EVENT_QUEUE = 16) provides drop-oldest backstop.
void AiEventProcessor::processReID(int camera_id,
                                   const nlohmann::json& obj,
                                   const cv::Mat& frame) {
    if (frame.empty()) return;

    int track_id = obj.value("track_id", -1);
    if (track_id < 0) {
        // bypass_tracker mode — no stable identity available.
        return;
    }

    // Optional confidence floor: ghost detections shouldn't pollute the
    // gallery. ReID accuracy degrades sharply on tiny / occluded persons, so
    // we mirror the intrusion gate (0.5) here.
    double confidence = obj.value("confidence", 0.0);
    if (confidence < 0.5) return;

    // bbox in worker emit shape is "box" (since 2026-04). Older producers used
    // "bbox". Accept both. Reject if neither is a 4-element array.
    auto j_bbox = obj.value("box",
                            obj.value("bbox", nlohmann::json::array()));
    if (!j_bbox.is_array() || j_bbox.size() < 4) return;

    int x1 = j_bbox[0].get<int>();
    int y1 = j_bbox[1].get<int>();
    int x2 = j_bbox[2].get<int>();
    int y2 = j_bbox[3].get<int>();

    // Clamp + sanity-check before crop so we never call cv::Mat(roi) with an
    // out-of-frame Rect (throws).
    x1 = std::max(0, std::min(x1, frame.cols - 1));
    y1 = std::max(0, std::min(y1, frame.rows - 1));
    x2 = std::max(0, std::min(x2, frame.cols));
    y2 = std::max(0, std::min(y2, frame.rows));
    if (x2 - x1 < 16 || y2 - y1 < 32) {
        // Smaller than 16×32 is too small for OSNet-class embedding networks
        // to produce meaningful features. Discard silently.
        return;
    }

    cv::Mat person_crop = frame(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
    if (person_crop.empty()) return;

    try {
        int global_id = ReIDEngine::getInstance().processDetection(camera_id, track_id, person_crop);
        if (global_id < 0) {
            // Engine not initialized / disabled / extract failed. Already
            // logged once at engine side. Don't spam per-frame.
            return;
        }
        // Successful match/insert. Don't LOG_INFO per frame — engine logs
        // the first observation and re-ID match events itself.
    } catch (const std::exception& e) {
        LOG_THROTTLED_WARN(5000, "AiEventProcessor::processReID exception: {}", e.what());
    }
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
