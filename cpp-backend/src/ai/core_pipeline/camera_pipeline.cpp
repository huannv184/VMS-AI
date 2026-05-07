#include "camera_pipeline.h"

// Include component headers
#include "video/video_source.h"
#include "video/video_source_factory.h"
#include "inference/multi_model_infer.h"
#include "inference/batch_inference_scheduler.h"
#include "inference/tracking.h"
#include "ipc/video_publisher.h"
#include "ipc/shared_memory_manager.h"
#include "common/frame.h"

#include <iostream>
#include <chrono>
#include <future>

namespace core {

CameraPipeline::CameraPipeline(const PipelineConfig& config)
    : config_(config)
{
}

CameraPipeline::~CameraPipeline() {
    stop();
}

bool CameraPipeline::init() {
    std::cout << "[Pipeline] Initializing Camera " << config_.camera_id << " (" << config_.rtsp_url << ")\n";

    // 1. Video Source
    video::SourceConfig src_config;
    src_config.type = video::SourceType::RTSP;
    src_config.uri = config_.rtsp_url;
    
    video_source_ = video::VideoSourceFactory::create(src_config);
    if (!video_source_) {
        std::cerr << "[Pipeline] Failed to create video source\n";
        return false;
    }

    // 2. Inference
    inference::MultiModelInfer::Config infer_config;
    infer_config.yolo_input_size = config_.yolo_input_size;
    infer_config.yolo_conf_threshold = config_.yolo_conf;
    infer_config.yolo_nms_threshold = config_.yolo_nms;
    infer_config.enable_face_detection = config_.scrfd_enabled;

    if (batch_scheduler_) {
        // YOLO runs through the shared batch scheduler — skip per-camera YOLO
        infer_config.enable_yolo = false;
        std::cout << "[Pipeline] Camera " << config_.camera_id
                  << ": YOLO delegated to BatchInferenceScheduler\n";
    } else {
        infer_config.yolo_model_path = config_.yolo_path;
    }

    multi_infer_ = std::make_unique<inference::MultiModelInfer>(infer_config);
    if (!multi_infer_->init()) {
        std::cerr << "[Pipeline] Failed to init inference\n";
        return false;
    }

    // 3. Tracking
    if (config_.tracking_enabled) {
        inference::TrackerConfig tracker_config;
        tracker_config.iou_threshold = config_.tracking_iou_threshold;
        tracker_config.max_age = config_.tracking_max_age;
        tracker_config.min_hits = config_.tracking_min_hits;
        tracker_config.use_reid = config_.tracking_use_reid;
        
        tracker_ = std::make_unique<inference::ObjectTracker>(tracker_config);
        
        if (config_.scrfd_enabled) {
            face_tracker_ = std::make_unique<inference::FaceTracker>(0.3f, 30, 3);
        }
    }

    // 4. Shared Memory (Optional, for compatibility)
    try {
        shm_manager_ = std::make_unique<ipc::SharedMemoryManager>(config_.camera_id);
        shm_manager_->initialize();
    } catch (...) {
        std::cerr << "[Pipeline] Warning: Failed to init shared memory\n";
    }
    
    // 5. Video Publisher (Optional, for compatibility)
    if (config_.video_stream_enabled) {
         video_publisher_ = std::make_unique<ipc::VideoPublisher>(
            config_.camera_id, config_.video_stream_endpoint, 30
        );
    }

    return true;
}

void CameraPipeline::start() {
    if (running_) return;
    
    // Open video source
    if (!video_source_->open()) {
        std::cerr << "[Pipeline] Failed to open RTSP stream\n";
        return;
    }

    running_ = true;
    thread_ = std::make_unique<std::thread>(&CameraPipeline::processingLoop, this);
    std::cout << "[Pipeline] Started processing thread for Camera " << config_.camera_id << "\n";
}

// Stop function fix
void CameraPipeline::stop() {
    running_ = false;
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    
    // Now safe to close source
    if (video_source_) {
        video_source_->close();
    }
}



PipelineStats CameraPipeline::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void CameraPipeline::processingLoop() {
    video::Frame frame;
    uint64_t frame_id = 0;
    
    const double TARGET_FPS = 15.0;
    const auto MIN_FRAME_TIME = std::chrono::microseconds(static_cast<int>(1000000.0 / TARGET_FPS));
    
    auto last_time = std::chrono::high_resolution_clock::now();
    
    int consecutive_failures = 0;

    while (running_) {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // 1. Read Frame
        if (!video_source_->read(frame)) {
            consecutive_failures++;
            if (consecutive_failures > 100) {
                 // Reconnect logic could go here
                 std::this_thread::sleep_for(std::chrono::seconds(1));
                 video_source_->open(); // Try reopen
                 consecutive_failures = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        consecutive_failures = 0;
        
        if (frame.image.empty()) continue;
        // Additional safe check
        if (frame.image.cols == 0 || frame.image.rows == 0) continue;
        
        frame_id++;

        // 2. Inference — batch path vs legacy path
        inference::MultiModelResult result;

        if (batch_scheduler_) {
            // Submit frame to shared YOLO batch scheduler → get future
            auto yolo_future = batch_scheduler_->submit(
                config_.camera_id, frame.image, static_cast<uint32_t>(frame_id));

            // Block until this frame's YOLO result is ready
            std::vector<inference::BBox> yolo_objects = yolo_future.get();

            // Run Face / Fire / LPR with pre-computed YOLO results
            result = multi_infer_->inferNonYolo(
                frame.image, static_cast<uint32_t>(frame_id), yolo_objects);
        } else {
            // Legacy: run all models sequentially on this thread
            result = multi_infer_->infer(
                frame.image, static_cast<uint32_t>(frame_id));
        }
        
        // 3. Tracking & Filtering
        std::vector<inference::TrackedObject> tracked_objects;
        
        
        std::vector<inference::BBox> valid_bboxes;
        for (const auto& obj : result.objects) {
            // Basic validation
            if (obj.x1 < 0 || obj.y1 < 0 || obj.x2 > frame.image.cols || obj.y2 > frame.image.rows) continue;
            
            // ✅ OPTIMIZATION: Filter to only person and vehicles  
            // class_id: 0=person, 2=car, 3=motorcycle, 5=bus, 7=truck
            if (obj.class_id != 0 && obj.class_id != 2 && obj.class_id != 3 && 
                obj.class_id != 5 && obj.class_id != 7) {
                continue;  // Skip all other 75 COCO classes
            }
            
            // Confidence filter
            if (obj.score >= config_.yolo_conf) {
                valid_bboxes.push_back(obj);
            }
        }

        if (tracker_) {
            tracked_objects = tracker_->update(valid_bboxes);
        } else {
            // No tracking, just pass through
            for (const auto& obj : valid_bboxes) {
                inference::TrackedObject t;
                t.bbox = obj;
                t.track_id = -1;
                t.label = obj.label;
                tracked_objects.push_back(t);
            }
        }
        
        // Face Tracking
        if (face_tracker_ && !result.faces.empty()) { // Use face tracker if enabled
             std::vector<inference::TrackedFace> tracked_faces = face_tracker_->update(result.faces);
             // Could map tracked IDs here if needed
        }

        // ✅ CRITIAL FIX: Add Faces to Output
        for (const auto& face : result.faces) {
            inference::TrackedObject to;
            to.bbox.x1 = face.x1;
            to.bbox.y1 = face.y1;
            to.bbox.x2 = face.x2;
            to.bbox.y2 = face.y2;
            to.confidence = face.confidence;
            to.bbox.score = face.confidence;
            to.bbox.class_id = -1; // Face
            
            if (!face.name.empty() && face.name != "Unknown") {
                to.label = face.name;
            } else {
                to.label = "Face";
            }
            
            to.track_id = face.person_id;
            tracked_objects.push_back(to);
        }

        // 4. Shared Memory Write — required for main backend to receive AI results
        if (shm_manager_) {
            shm_manager_->writeMetadata(result, tracked_objects, config_.camera_id,
                                        frame.image.cols, frame.image.rows);
        }

        // 5. Callback (Zero-Copy Broadcast)
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (frame_callback_) {
                frame_callback_(frame.image, result, tracked_objects);
            }
        }
        
        // 5. Update Stats
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.frame_count = frame_id;
            stats_.objects_detected = static_cast<uint32_t>(result.objects.size());
        }
        
        // 6. Rate Limiting (15 FPS)
        auto end_time = std::chrono::high_resolution_clock::now();
        auto processing_time = end_time - start_time;
        if (processing_time < MIN_FRAME_TIME) {
            std::this_thread::sleep_for(MIN_FRAME_TIME - processing_time);
        }
    }
}

} // namespace core
