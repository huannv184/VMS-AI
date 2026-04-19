#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <vector>
#include <future>
#include <opencv2/core/mat.hpp>

// Forward declarations
namespace video { class VideoSource; struct Frame; }
namespace inference { class MultiModelInfer; class ObjectTracker; class FaceTracker; class BatchInferenceScheduler; struct MultiModelResult; struct TrackedObject; struct TrackedFace; }
namespace ipc { class VideoPublisher; class SharedMemoryManager; struct ShmFrameMetadata; }

namespace core {

struct PipelineConfig {
    int camera_id = 0;
    std::string rtsp_url;
    
    // AI Config
    std::string yolo_path = "inference/models/yolo11m.engine";
    int yolo_input_size = 640;
    float yolo_conf = 0.30f;
    float yolo_nms = 0.45f;
    
    bool scrfd_enabled = false;
    std::string scrfd_path = "models/scrfd_2.5g_bnkps.trt";
    
    bool arcface_enabled = false;
    std::string arcface_path = "models/arcfaceresnet100-8.trt";
    
    // Tracking
    bool tracking_enabled = true;
    float tracking_iou_threshold = 0.3f;
    int tracking_max_age = 30;
    int tracking_min_hits = 3;
    bool tracking_use_reid = false;
    
    // Streaming (Legacy ZMQ, can be disabled for in-process)
    bool video_stream_enabled = false; 
    std::string video_stream_endpoint;
};

struct PipelineStats {
    double fps = 0.0;
    uint64_t frame_count = 0;
    uint32_t objects_detected = 0;
};

using FrameCallback = std::function<void(const cv::Mat& frame, 
                                           const inference::MultiModelResult& result,
                                           const std::vector<inference::TrackedObject>& tracked_objects)>;

class CameraPipeline {
public:
    CameraPipeline(const PipelineConfig& config);
    ~CameraPipeline();

    bool init();
    void start();
    void stop();
    
    bool isRunning() const { return running_; }
    PipelineStats getStats() const;
    
    // Set shared batch scheduler (call before start()).
    // When set, YOLO runs through the batch scheduler; per-camera YOLO is skipped.
    void setBatchScheduler(inference::BatchInferenceScheduler* scheduler) {
        batch_scheduler_ = scheduler;
    }

    // Set callback for processed frames (DIRECT CALL - FASTEST)
    void setFrameCallback(FrameCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        frame_callback_ = callback;
    }

private:
    void processingLoop();

    PipelineConfig config_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> thread_;
    
    // Components
    std::unique_ptr<video::VideoSource> video_source_;
    std::unique_ptr<inference::MultiModelInfer> multi_infer_;
    std::unique_ptr<inference::ObjectTracker> tracker_;
    std::unique_ptr<inference::FaceTracker> face_tracker_;

    // Shared batch scheduler (non-owning — lifetime managed externally)
    inference::BatchInferenceScheduler* batch_scheduler_ = nullptr;
    
    // Optional Legacy Components
    std::unique_ptr<ipc::VideoPublisher> video_publisher_;
    std::unique_ptr<ipc::SharedMemoryManager> shm_manager_;

    // Callbacks
    FrameCallback frame_callback_;
    std::mutex callback_mutex_;
    
    // Stats
    mutable std::mutex stats_mutex_;
    PipelineStats stats_;
};

} // namespace core
