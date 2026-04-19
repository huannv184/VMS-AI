// pipeline/include/pipeline/pipeline_manager.h
#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <vector>
#include <string>
#include <chrono>

#include "inference/advanced_infer.h"
#include "inference/bounding_box.h"

// Forward declarations
namespace video {
    class VideoPipeline;
    class VideoSource;
    struct Frame;  // <-- THÊM DÒNG NÀY
}

namespace video {

enum class ManagerState {
    IDLE,
    RUNNING,
    PAUSED,
    STOPPED,
    ERROR
};

struct PipelineStats {
    uint64_t total_frames = 0;
    uint64_t processed_frames = 0;
    uint64_t dropped_frames = 0;
    double fps = 0.0;
    double avg_latency_ms = 0.0;
    std::string last_error;
};

class PipelineManager {
public:
    PipelineManager();
    ~PipelineManager();

    // Initialize pipeline with video source and inference engine
    bool initialize(std::unique_ptr<VideoSource> source,
                    std::shared_ptr<inference::AdvancedInfer> inference);

    // Control methods
    bool start();
    bool pause();
    bool resume();
    bool stop();

    // Status & metrics
    ManagerState getState() const;
    PipelineStats getStats() const;
    inference::InferenceMetrics getInferenceMetrics() const;

    // Callback for detection results
    void setDetectionCallback(
        std::function<void(const Frame&, const std::vector<inference::BBox>&)> callback);

private:
    void processingLoop();
    void updateStats(double latency_ms);

private:
    std::unique_ptr<VideoPipeline> pipeline_;
    std::shared_ptr<inference::AdvancedInfer> inference_;

    std::atomic<ManagerState> state_;
    PipelineStats stats_;

    std::thread worker_;
    mutable std::mutex mutex_;

    std::function<void(const Frame&, const std::vector<inference::BBox>&)> callback_;

    std::chrono::steady_clock::time_point start_time_;
};

} // namespace video