// pipeline/src/pipeline_manager.cpp
#include "pipeline_manager.h"
#include "video_pipeline.h"
#include "video/video_source.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>

namespace video {

PipelineManager::PipelineManager() 
    : state_(ManagerState::IDLE) {
}

PipelineManager::~PipelineManager() {
    stop();
}

bool PipelineManager::initialize(
    std::unique_ptr<VideoSource> source,
    std::shared_ptr<inference::AdvancedInfer> inference) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ != ManagerState::IDLE && state_ != ManagerState::STOPPED) {
        std::cerr << "[PipelineManager] Cannot initialize while running" << std::endl;
        return false;
    }
    
    std::cout << "[PipelineManager] Initializing..." << std::endl;
    
    inference_ = inference;
    pipeline_ = std::make_unique<VideoPipeline>(std::move(source));
    
    if (!pipeline_->open()) {
        std::cerr << "[PipelineManager] Failed to open video pipeline" << std::endl;
        state_ = ManagerState::ERROR;
        stats_.last_error = "Failed to open video pipeline";
        return false;
    }
    
    state_ = ManagerState::IDLE;
    std::cout << "[PipelineManager] Initialized successfully" << std::endl;
    
    return true;
}

bool PipelineManager::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ != ManagerState::IDLE && state_ != ManagerState::STOPPED) {
        std::cerr << "[PipelineManager] Already running" << std::endl;
        return false;
    }
    
    if (!pipeline_ || !inference_) {
        std::cerr << "[PipelineManager] Not initialized" << std::endl;
        return false;
    }
    
    std::cout << "[PipelineManager] Starting..." << std::endl;
    
    stats_ = PipelineStats();
    start_time_ = std::chrono::steady_clock::now();
    state_ = ManagerState::RUNNING;
    
    worker_ = std::thread(&PipelineManager::processingLoop, this);
    
    return true;
}

bool PipelineManager::pause() {
    if (state_ != ManagerState::RUNNING) {
        return false;
    }
    
    std::cout << "[PipelineManager] Pausing..." << std::endl;
    state_ = ManagerState::PAUSED;
    return true;
}

bool PipelineManager::resume() {
    if (state_ != ManagerState::PAUSED) {
        return false;
    }
    
    std::cout << "[PipelineManager] Resuming..." << std::endl;
    state_ = ManagerState::RUNNING;
    return true;
}

bool PipelineManager::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (state_ == ManagerState::IDLE || state_ == ManagerState::STOPPED) {
            return true;
        }
        
        std::cout << "[PipelineManager] Stopping..." << std::endl;
        state_ = ManagerState::STOPPED;
    }
    
    if (worker_.joinable()) {
        worker_.join();
    }
    
    if (pipeline_) {
        pipeline_->close();
    }
    
    std::cout << "[PipelineManager] Stopped" << std::endl;
    return true;
}

ManagerState PipelineManager::getState() const {
    return state_.load();
}

PipelineStats PipelineManager::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

inference::InferenceMetrics PipelineManager::getInferenceMetrics() const {
    if (!inference_) {
        return {};
    }
    return inference_->getMetrics();
}

void PipelineManager::setDetectionCallback(
    std::function<void(const Frame&, const std::vector<inference::BBox>&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
}

void PipelineManager::processingLoop() {
    std::cout << "[PipelineManager] Processing loop started" << std::endl;
    
    Frame frame;
    int consecutive_failures = 0;
    const int MAX_FAILURES = 100;
    
    while (state_ == ManagerState::RUNNING || state_ == ManagerState::PAUSED) {
        while (state_ == ManagerState::PAUSED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (state_ == ManagerState::STOPPED) {
            break;
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        
        if (!pipeline_->read(frame)) {
            consecutive_failures++;
            if (consecutive_failures >= MAX_FAILURES) {
                std::cerr << "[PipelineManager] Too many consecutive failures" << std::endl;
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = ManagerState::ERROR;
                stats_.last_error = "Too many consecutive frame read failures";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        consecutive_failures = 0;
        stats_.total_frames++;
        
        auto detections = inference_->detectObjects(frame.image);
        
        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        
        updateStats(latency);
        stats_.processed_frames++;
        
        if (callback_) {
            callback_(frame, detections);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "[PipelineManager] Processing loop ended" << std::endl;
}

void PipelineManager::updateStats(double latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    double alpha = 0.1;
    stats_.avg_latency_ms = alpha * latency_ms + (1 - alpha) * stats_.avg_latency_ms;
    
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time_).count();
    if (elapsed > 0) {
        stats_.fps = stats_.processed_frames / elapsed;
    }
}

} // namespace video
