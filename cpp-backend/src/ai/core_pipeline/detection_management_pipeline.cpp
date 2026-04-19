#include "event_writer_pipeline.h"
#include "detection_management_pipeline.h"
#include "pipeline_manager.h"
#include "tracking_pipeline.h"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace pipeline {

// ============================================================================
// DetectionManagementPipeline Implementation
// ============================================================================

DetectionManagementPipeline::DetectionManagementPipeline()
    : total_frames_(0)
    , start_time_(std::chrono::steady_clock::now())
{
}

void DetectionManagementPipeline::updateStats(const DetectionResult& result) {
    total_frames_++;
    
    // Update object statistics
    stats_.total_detections += result.objects.size();
    if (!result.objects.empty()) {
        stats_.frames_with_detections++;
    }
    
    // Update face statistics
    stats_.total_faces += result.faces.size();
    if (!result.faces.empty()) {
        stats_.frames_with_faces++;
    }
    
    // Update class counts
    for (const auto& obj : result.objects) {
        stats_.class_counts[obj.class_id]++;
    }
}

void DetectionManagementPipeline::printStats() const {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_
    ).count();
    
    std::cout << "\n========================================\n";
    std::cout << "     DETECTION STATISTICS\n";
    std::cout << "========================================\n";
    std::cout << "Total frames:           " << total_frames_ << "\n";
    std::cout << "Runtime:                " << elapsed << " seconds\n";
    std::cout << "Frames with detections: " << stats_.frames_with_detections << "\n";
    std::cout << "Frames with faces:      " << stats_.frames_with_faces << "\n";
    std::cout << "Total detections:       " << stats_.total_detections << "\n";
    std::cout << "Total faces:            " << stats_.total_faces << "\n";
    std::cout << "Avg detections/frame:   " << getAvgDetectionsPerFrame() << "\n";
    std::cout << "Avg faces/frame:        " << getAvgFacesPerFrame() << "\n";
    
    if (!stats_.class_counts.empty()) {
        std::cout << "\nClass Distribution:\n";
        for (const auto& [class_id, count] : stats_.class_counts) {
            std::cout << "  Class " << class_id << ": " << count << "\n";
        }
    }
    
    std::cout << "========================================\n\n";
}

void DetectionManagementPipeline::reset() {
    stats_ = DetectionStats();
    total_frames_ = 0;
    start_time_ = std::chrono::steady_clock::now();
}

double DetectionManagementPipeline::getAvgDetectionsPerFrame() const {
    if (total_frames_ == 0) return 0.0;
    return static_cast<double>(stats_.total_detections) / total_frames_;
}

double DetectionManagementPipeline::getAvgFacesPerFrame() const {
    if (total_frames_ == 0) return 0.0;
    return static_cast<double>(stats_.total_faces) / total_frames_;
}

// ============================================================================
// Filtering Utilities Implementation
// ============================================================================

std::vector<inference::BoundingBox> filterDetections(
    const std::vector<inference::BoundingBox>& detections,
    float confidence_threshold)
{
    std::vector<inference::BoundingBox> filtered;
    filtered.reserve(detections.size());
    
    for (const auto& det : detections) {
        if (det.score >= confidence_threshold) {
            filtered.push_back(det);
        }
    }
    
    return filtered;
}

float calculateIoU(const inference::BoundingBox& a, const inference::BoundingBox& b) {
    float x1 = std::max(a.x1, b.x1);
    float y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2);
    float y2 = std::min(a.y2, b.y2);
    
    if (x2 <= x1 || y2 <= y1) {
        return 0.0f;
    }
    
    float intersection = (x2 - x1) * (y2 - y1);
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float union_area = area_a + area_b - intersection;
    
    return union_area > 0 ? intersection / union_area : 0.0f;
}

bool isFaceInsideObject(
    const inference::BoundingBox& face,
    const inference::BoundingBox& object,
    float min_overlap)
{
    float x1 = std::max(face.x1, object.x1);
    float y1 = std::max(face.y1, object.y1);
    float x2 = std::min(face.x2, object.x2);
    float y2 = std::min(face.y2, object.y2);
    
    if (x2 <= x1 || y2 <= y1) {
        return false;
    }
    
    float intersection = (x2 - x1) * (y2 - y1);
    float face_area = (face.x2 - face.x1) * (face.y2 - face.y1);
    
    return (intersection / face_area) >= min_overlap;
}

std::pair<std::vector<inference::BoundingBox>, std::vector<bool>> 
smartFilterWithFaces(
    const std::vector<inference::BoundingBox>& objects,
    const std::vector<inference::BoundingBox>& faces,
    int frame_width,
    int frame_height,
    float confidence_threshold)
{
    std::vector<inference::BoundingBox> filtered_objects;
    std::vector<bool> has_face_flags;
    
    const float min_size_ratio = 0.02f;
    const float max_size_ratio = 0.95f;
    const float edge_margin = 0.05f;
    
    float min_size = std::min(frame_width, frame_height) * min_size_ratio;
    float max_area = frame_width * frame_height * max_size_ratio;
    float edge_threshold_x = frame_width * edge_margin;
    float edge_threshold_y = frame_height * edge_margin;
    
    for (const auto& obj : objects) {
        // Confidence check
        if (obj.score < confidence_threshold) {
            continue;
        }
        
        // Size check
        float width = obj.x2 - obj.x1;
        float height = obj.y2 - obj.y1;
        float area = width * height;
        
        if (width < min_size || height < min_size || area > max_area) {
            continue;
        }
        
        // Edge check
        bool on_edge = (obj.x1 < edge_threshold_x || 
                       obj.x2 > frame_width - edge_threshold_x ||
                       obj.y1 < edge_threshold_y || 
                       obj.y2 > frame_height - edge_threshold_y);
        
        // Check if object has face
        bool has_face = false;
        for (const auto& face : faces) {
            if (isFaceInsideObject(face, obj, 0.5f)) {
                has_face = true;
                break;
            }
        }
        
        // Priority: objects with faces OR not on edge
        if (has_face || !on_edge) {
            filtered_objects.push_back(obj);
            has_face_flags.push_back(has_face);
        }
    }
    
    return {filtered_objects, has_face_flags};
}

} // namespace pipeline
