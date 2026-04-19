#pragma once
#include "ObjectTube.h"
#include <vector>
#include <map>
#include <mutex>

namespace ai {
namespace synopsis {

class TubeManager {
public:
    TubeManager();
    ~TubeManager();

    // Add a detection to the manager to potentially update existing tubes or create new ones
    // frameTime: current frame timestamp in ms
    // detections: list of bounding boxes and class IDs
    // frame: current video frame (to extract crops)
    void processFrame(long long frameTime, const std::vector<cv::Rect>& boxes, 
                      const std::vector<int>& classIds, const cv::Mat& frame);

    // Finalize tubes (e.g., when video ends), removing short/invalid tubes
    void finalize();

    // accessors
    const std::vector<ObjectTube>& getTubes() const { return tubes_; }

private:
    std::vector<ObjectTube> tubes_;
    std::map<int, int> activeTubeIndices_; // Track ID -> Index in tubes_ vector
    int nextTubeId_ = 0;
    
    // Config
    int maxDropoutFrames_ = 5; // How many frames an object can be missing before tube is closed
    float iouThreshold_ = 0.3f; // IoU to match object across frames
    int minTubeDuration_ = 1000; // Minimum duration (ms) to keep a tube
    
    // Memory Explosion Fixes
    size_t maxTubeFrames_ = 150; // Max frames per tube (approx 5-10s at stride)
    int frameStride_ = 3;        // Only store 1 in every 3 frames
    int maxCropArea_ = 256 * 256; // Max pixels for crop (64KB). Downscale if larger.
};

} // namespace synopsis
} // namespace ai
