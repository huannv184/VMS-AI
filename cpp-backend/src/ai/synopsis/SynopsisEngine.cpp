#include "SynopsisEngine.h"
#include "TubeManager.h"
#include <spdlog/spdlog.h>
#include <filesystem>

// Assuming we have YOLO available or we use simple MOG2 for detection if needed
// For Phase 2, we will assume we can get detections from a sidecar file OR run detection.
// Given the "Motion Summary" title, we should probably run simple motion detection (MOG2)
// if we don't have metadata, to support any video.

namespace ai {
namespace synopsis {

SynopsisEngine::SynopsisEngine() {}
SynopsisEngine::~SynopsisEngine() {}

bool SynopsisEngine::generate(const SynopsisConfig& config, std::function<void(float)> progressCallback) {
    if (!std::filesystem::exists(config.inputVideoPath)) {
        spdlog::error("Synopsis input video not found: {}", config.inputVideoPath);
        return false;
    }

    spdlog::info("Starting synopsis generation for: {}", config.inputVideoPath);

    cv::VideoCapture cap(config.inputVideoPath);
    if (!cap.isOpened()) return false;

    double fps = cap.get(cv::CAP_PROP_FPS);
    int width = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int height = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    int totalFrames = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);

    // 1. Generate Background (Simple average of first few frames or every Nth frame)
    if (progressCallback) progressCallback(0.05f);
    cv::Mat background = generateBackground(config.inputVideoPath);
    if (background.empty()) {
         spdlog::error("Failed to generate background");
         return false;
    }

    // 2. Extract Tubes (using MOG2 for robust motion detection)
    TubeManager tubeMgr;
    auto mog2 = cv::createBackgroundSubtractorMOG2(500, 16, true);
    
    cap.set(cv::CAP_PROP_POS_FRAMES, 0);
    cv::Mat frame, fgMask;
    int frameIdx = 0;

    while (cap.read(frame)) {
        long long timestamp = (long long)(frameIdx * 1000.0 / fps);
        
        // MOG2
        mog2->apply(frame, fgMask);
        
        // Find contours
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(fgMask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        std::vector<cv::Rect> boxes;
        std::vector<int> classIds; // Dummy class ID 0 for motion
        
        for (const auto& cnt : contours) {
            if (cv::contourArea(cnt) > 500) { // Min area
                boxes.push_back(cv::boundingRect(cnt));
                classIds.push_back(0); 
            }
        }

        tubeMgr.processFrame(timestamp, boxes, classIds, frame);

        frameIdx++;
        if (frameIdx % 100 == 0 && progressCallback) {
            float p = 0.1f + 0.4f * ((float)frameIdx / totalFrames); // 10% to 50%
            progressCallback(p);
        }
    }
    tubeMgr.finalize();
    std::vector<ObjectTube> tubes = tubeMgr.getTubes();

    if (tubes.empty()) {
        spdlog::warn("No motion found in video");
        // Output just background?
    }

    // 3. Schedule Tubes
    if (progressCallback) progressCallback(0.6f);
    scheduleTubes(tubes, config.targetDurationSec * 1000);

    // 4. Render
    if (progressCallback) progressCallback(0.7f);
    renderSynopsis(config.outputVideoPath, background, tubes, width, height, fps, config.targetDurationSec * 1000);

    if (progressCallback) progressCallback(1.0f);
    spdlog::info("Synopsis generation complete");
    return true;
}

cv::Mat SynopsisEngine::generateBackground(const std::string& videoPath) {
    cv::VideoCapture cap(videoPath);
    cv::Mat frame, sum, avg;
    int count = 0;
    int maxFrames = 50; // Use 50 frames spread out

    // Simple uniform sampling
    int total = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
    int step = std::max(1, total / maxFrames);

    for (int i = 0; i < total; i += step) {
        cap.set(cv::CAP_PROP_POS_FRAMES, i);
        if (cap.read(frame)) {
            if (sum.empty()) {
                frame.convertTo(sum, CV_32F);
            } else {
                cv::Mat tmp;
                frame.convertTo(tmp, CV_32F);
                sum += tmp;
            }
            count++;
        }
    }

    if (count > 0) {
        sum /= count;
        sum.convertTo(avg, CV_8U);
    }
    return avg;
}

void SynopsisEngine::scheduleTubes(std::vector<ObjectTube>& tubes, int targetDurationMs) {
    // Simple greedy scheduling:
    // Sort tubes by "activity" or just original time?
    // We want to pack them.
    // Algorithm: shift start time of each tube to smallest possible time t such that 
    // it doesn't spatially overlap with any already scheduled tube at that time.
    
    // For simplicity V1: Just shift all to start at 0, stacking them heavily.
    // Better V1.5: Random start times within [0, targetDuration - tubeDuration]
    
    for (auto& tube : tubes) {
        long long dur = tube.duration();
        if (targetDurationMs > dur) {
            // Random start
            tube.scheduledStartTime = rand() % (targetDurationMs - (int)dur);
        } else {
            tube.scheduledStartTime = 0; // Tube longer than video
        }
    }
}

void SynopsisEngine::renderSynopsis(const std::string& outputPath, const cv::Mat& background, 
                                    const std::vector<ObjectTube>& tubes, int width, int height, double fps, int targetDurationMs) {
    
    cv::VideoWriter writer(outputPath, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps, cv::Size(width, height));
    
    if (!writer.isOpened()) {
        spdlog::error("Could not open output video for write: {}", outputPath);
        return;
    }

    int totalFrames = (int)(targetDurationMs * fps / 1000.0);
    
    for (int iframe = 0; iframe < totalFrames; ++iframe) {
        long long currentTime = (long long)(iframe * 1000.0 / fps);
        
        cv::Mat frame = background.clone();

        // Overlay active tubes
        for (const auto& tube : tubes) {
            long long relTime = currentTime - tube.scheduledStartTime;
            if (relTime >= 0 && relTime < tube.duration()) {
                // Find frame in tube corresponding to relTime
                // Since tube.frames are linear and captured at original FPS approx, 
                // we can map relTime to original tube time offset
                
                // Find closest frame
                long long originalOffset = relTime; 
                // We stored absolute timestamp in TubeFrame.
                // Tube start time = tube.startTime.
                // TubeFrame time - tube.startTime = offset
                
                // Use simple linear search or closest
                 for (const auto& tf : tube.frames) {
                     // Check if this frame is "current" for the tube playback
                     // Current playback time of tube = relTime
                     long long tfOffset = tf.timestamp - tube.startTime;
                     long long diff = std::abs(tfOffset - originalOffset);
                     
                     if (diff < (1000.0/fps)) { // Match within 1 frame duration
                         // Draw tf.crop at tf.rect
                         // Handle boundaries
                         cv::Mat roi = frame(tf.rect);
                         // Simple copy, ideally use mask
                         tf.crop.copyTo(roi);
                         
                         // Add bounding box or label if needed
                         // cv::rectangle(frame, tf.rect, cv::Scalar(0,255,0), 2);
                         
                         // Add timestamp text
                         // std::string timeStr = ...
                         break; // Draw once per tube per frame
                     }
                 }
            }
        }
        
        writer.write(frame);
    }
    
    writer.release();
}

} // namespace synopsis
} // namespace ai
