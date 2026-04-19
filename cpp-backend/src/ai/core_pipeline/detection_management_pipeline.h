#pragma once

#include <vector>
#include <map>
#include <string>
#include <chrono>
#include "inference/bounding_box.h"

namespace pipeline {

// ============================================================================
// Detection Statistics
// ============================================================================

struct DetectionStats {
    uint32_t total_detections = 0;
    uint32_t total_faces = 0;
    uint32_t frames_with_detections = 0;
    uint32_t frames_with_faces = 0;
    std::map<int, uint32_t> class_counts;
};

struct DetectionResult {
    std::vector<inference::BoundingBox> objects;
    std::vector<inference::BoundingBox> faces;
    uint32_t frame_id = 0;
    uint64_t timestamp_ms = 0;
};

// ============================================================================
// Detection Management Pipeline
// ============================================================================

class DetectionManagementPipeline {
public:
    DetectionManagementPipeline();
    ~DetectionManagementPipeline() = default;

    // Update statistics with new detection result
    void updateStats(const DetectionResult& result);
    
    // Print current statistics
    void printStats() const;
    
    // Get statistics
    const DetectionStats& getStats() const { return stats_; }
    
    // Reset statistics
    void reset();
    
    // Get average detections per frame
    double getAvgDetectionsPerFrame() const;
    
    // Get average faces per frame
    double getAvgFacesPerFrame() const;

private:
    DetectionStats stats_;
    uint32_t total_frames_;
    std::chrono::steady_clock::time_point start_time_;
};

// ============================================================================
// Filtering Utilities
// ============================================================================

// Filter detections by confidence threshold
std::vector<inference::BoundingBox> filterDetections(
    const std::vector<inference::BoundingBox>& detections,
    float confidence_threshold
);

// Smart filter with face priority
std::pair<std::vector<inference::BoundingBox>, std::vector<bool>> 
smartFilterWithFaces(
    const std::vector<inference::BoundingBox>& objects,
    const std::vector<inference::BoundingBox>& faces,
    int frame_width,
    int frame_height,
    float confidence_threshold
);

// Calculate IoU (Intersection over Union)
float calculateIoU(const inference::BoundingBox& a, const inference::BoundingBox& b);

// Check if face is inside object bbox
bool isFaceInsideObject(
    const inference::BoundingBox& face,
    const inference::BoundingBox& object,
    float min_overlap = 0.5f
);

} // namespace pipeline
