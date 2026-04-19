#pragma once

#ifndef AIWORKER_H
#define AIWORKER_H

// Prevent Windows macro pollution
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <thread>
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <chrono>
#include <functional> // Added for std::function
#include <opencv2/core.hpp> // Added for cv::Mat
#include <zmq.hpp> // Added for ZMQ commands

// Forward declarations to avoid heavy includes in header
namespace video { class VideoSource; }
namespace inference { 
    class MultiModelInfer; 
    class ObjectTracker;
    class FaceTracker;
    // Resolve incomplete types by including headers or defining structs if small
    // Actually, including the headers is safer here as they are used in std::function arguments
}
#include "inference/multi_model_infer.h" // For MultiModelResult
#include "inference/tracking.h"          // For TrackedObject

namespace ipc { 
    class SharedMemoryManager;
    class VideoPublisher;
}

/**
 * AIWorker - Thread-safe AI processing worker for video streams
 * 
 * Features:
 * - Automatic model path resolution
 * - State tracking and error reporting
 * - Statistics monitoring
 * - Graceful shutdown
 * - Reconnection on video source failure
 */
class AIWorker {
public:
    // Worker states
    enum class State {
        STOPPED,        // Not running
        INITIALIZING,   // Starting up, loading models
        RUNNING,        // Processing frames normally
        FAILED,         // Fatal error occurred (renamed from ERROR to avoid Windows macro)
        RECONNECTING    // Attempting to reconnect video source
    };

    // Configuration structure
    struct Config {
        // Camera settings
        std::string rtsp_url;
        int camera_id = 0;
        
        // Model paths (can be relative or absolute, will auto-resolve)
        std::string yolo_path = "yolo11m.engine";
        int yolo_input_size = 640;
        float yolo_conf = 0.30f;
        float yolo_nms = 0.45f;
        
        // Face detection (optional)
        bool scrfd_enabled = false;
        std::string scrfd_path = "scrfd_2.5g_bnkps.trt";
        
        // Tracking settings
        bool tracking_enabled = true;
        float tracking_iou_threshold = 0.3f;
        int tracking_max_age = 30;
        int tracking_min_hits = 3;
        bool tracking_use_reid = false;
        
        // Video streaming
        bool video_stream_enabled = true;
        std::string video_stream_endpoint = "tcp://127.0.0.1:5555";
        
        // Performance
        double target_fps = 15.0;  // Inference FPS target
        int max_reconnect_attempts = 100;
        int reconnect_delay_sec = 5;
    };

    // Runtime statistics
    struct Statistics {
        uint64_t frames_processed = 0;
        uint64_t frames_dropped = 0;
        double current_fps = 0.0;
        double inference_fps = 0.0;
        int active_tracks = 0;
        uint32_t consecutive_failures = 0;
        uint64_t total_inference_time_ms = 0;
        uint64_t uptime_seconds = 0;
    };

    // Constructor & Destructor
    explicit AIWorker(const Config& config);
    ~AIWorker();

    // Delete copy/move to prevent issues
    AIWorker(const AIWorker&) = delete;
    AIWorker& operator=(const AIWorker&) = delete;

    // Control methods
    void start();
    void stop();
    
    // State queries (thread-safe)
    bool isRunning() const { return m_running.load(); }
    State getState() const { return m_state.load(); }
    std::string getLastError() const;
    Statistics getStatistics() const;

private:
    // Main processing loop
    void run();
    
    // Helper methods
    void setError(const std::string& error);
    void updateStatistics(double fps, double infer_fps, int tracks);
    std::string resolveModelPath(const std::string& path, const std::string& filename);

    // Configuration
    Config m_config;
    
    // State management (atomic for thread-safety)
    std::atomic<bool> m_running{false};
    std::atomic<State> m_state{State::STOPPED};
    std::chrono::steady_clock::time_point m_startTime;
    
    // Error tracking (mutex-protected)
    mutable std::mutex m_errorMutex;
    std::string m_lastError;
    
    // Statistics (mutex-protected)
    mutable std::mutex m_statsMutex;
    Statistics m_statistics;
    
    // ZMQ Command Socket (for embedding requests)
    std::unique_ptr<zmq::context_t> m_zmqContext;
    std::unique_ptr<zmq::socket_t> m_commandSocket;
    void handleCommands();
    
    // Worker thread
    std::thread m_thread;

    // AI components (managed by unique_ptr for RAII)
    std::unique_ptr<video::VideoSource> m_videoSource;
    std::unique_ptr<inference::MultiModelInfer> m_multiInfer;
    std::unique_ptr<inference::ObjectTracker> m_tracker;
    std::unique_ptr<inference::FaceTracker> m_faceTracker;
    // std::unique_ptr<ipc::SharedMemoryManager> m_shmManager; // REMOVED
    std::unique_ptr<ipc::VideoPublisher> m_videoPublisher;

public:
    // Callback definition: Frame, Inference Result, Tracked Objects
    using FrameCallback = std::function<void(const cv::Mat&, const inference::MultiModelResult&, const std::vector<inference::TrackedObject>&)>;
    
    void setFrameCallback(FrameCallback callback) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_frameCallback = callback;
    }

private:
    std::mutex m_callbackMutex;
    FrameCallback m_frameCallback;
};

#endif // AIWORKER_H