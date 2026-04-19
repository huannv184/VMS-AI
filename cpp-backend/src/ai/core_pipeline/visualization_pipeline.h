#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>
#include "tracking_pipeline.h"  // ← SỬA: bỏ "pipeline/"
#include "inference/bounding_box.h"

namespace pipeline {

// ============================================================================
// Visualization Configuration
// ============================================================================

struct VisualizationConfig {
    // Colors
    cv::Scalar person_color = cv::Scalar(0, 255, 255);       // Yellow
    cv::Scalar person_with_face_color = cv::Scalar(0, 255, 0); // Green
    cv::Scalar face_color = cv::Scalar(255, 0, 0);           // Blue
    cv::Scalar stable_track_color = cv::Scalar(0, 255, 0);   // Green
    
    // Thresholds
    int stable_track_threshold = 10;  // Frames to consider track stable
    float face_overlap_threshold = 0.5f;
    
    // Drawing options
    bool draw_track_ids = true;
    bool draw_confidence = true;
    bool draw_class_names = true;
    bool draw_face_boxes = true;
    bool draw_info_overlay = true;
    bool draw_velocity_vectors = false;
    
    // Text options
    double font_scale = 0.5;
    int font_thickness = 2;
    cv::HersheyFonts font_face = cv::FONT_HERSHEY_SIMPLEX;
    
    // Box thickness
    int normal_thickness = 2;
    int stable_thickness = 3;
    int face_thickness = 1;
};

// ============================================================================
// Visualization Pipeline
// ============================================================================

class VisualizationPipeline {
public:
    explicit VisualizationPipeline(const VisualizationConfig& config = VisualizationConfig());
    ~VisualizationPipeline() = default;

    // Draw tracks on frame
    void drawTracks(
        cv::Mat& frame,
        const std::vector<Track>& tracks
    );
    
    // Draw face boxes
    void drawFaces(
        cv::Mat& frame,
        const std::vector<inference::BoundingBox>& faces,
        const std::vector<Track>& tracks
    );
    
    // Draw info overlay
    void drawInfoOverlay(
        cv::Mat& frame,
        double fps,
        uint32_t frame_count,
        size_t object_count,
        size_t face_count,
        float object_confidence_threshold,
        float face_confidence_threshold,
        double latency_ms
    );
    
    // Draw complete visualization
    cv::Mat visualize(
        const cv::Mat& frame,
        const std::vector<Track>& tracks,
        const std::vector<inference::BoundingBox>& faces,
        double fps,
        uint32_t frame_count,
        double latency_ms = 0.0
    );
    
    // Update configuration
    void setConfig(const VisualizationConfig& config) { config_ = config; }
    const VisualizationConfig& getConfig() const { return config_; }

private:
    // Helper: Draw single track
    void drawSingleTrack(
        cv::Mat& frame,
        const Track& track
    );
    
    // Helper: Check if face is inside track
    bool isFaceInsideTrack(
        const inference::BoundingBox& face,
        const Track& track
    );
    
    // Helper: Draw text with background
    void drawTextWithBackground(
        cv::Mat& frame,
        const std::string& text,
        cv::Point position,
        cv::Scalar text_color,
        cv::Scalar bg_color
    );

private:
    VisualizationConfig config_;
};

// ============================================================================
// Utility Functions
// ============================================================================

// Create color from track ID (consistent coloring)
cv::Scalar getColorFromTrackId(int track_id);

// Draw FPS counter
void drawFPSCounter(cv::Mat& frame, double fps, cv::Point position = cv::Point(10, 30));

// Draw latency info
void drawLatencyInfo(cv::Mat& frame, double latency_ms, cv::Point position = cv::Point(10, 60));

// Draw detection count
void drawDetectionCount(
    cv::Mat& frame,
    size_t object_count,
    size_t face_count,
    cv::Point position = cv::Point(10, 90)
);

// Create semi-transparent overlay
void createOverlay(
    cv::Mat& frame,
    cv::Rect region,
    cv::Scalar color,
    double alpha = 0.3
);

} // namespace pipeline