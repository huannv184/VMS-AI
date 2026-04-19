#pragma once

#include <vector>
#include <map>
#include <string>
#include <opencv2/opencv.hpp>
#include "inference/bounding_box.h"

namespace pipeline {

// ============================================================================
// Tracking Structures
// ============================================================================

struct Track {
    int track_id;
    cv::Rect bbox;
    float confidence;
    int class_id;
    std::string class_name;
    bool has_face;
    int age;                    // How many frames this track has been alive
    int stable_frames;          // Consecutive frames with good detection
    int lost_frames;            // Consecutive frames without detection
    cv::Point2f velocity;       // Estimated velocity for prediction
};

// ============================================================================
// Simple Tracking Pipeline
// ============================================================================

class TrackingPipeline {
public:
    TrackingPipeline(
        float iou_threshold = 0.3f,
        int max_lost_frames = 30,
        int min_stable_frames = 5
    );
    
    ~TrackingPipeline() = default;

    // Update tracks with new detections
    std::vector<Track> update(
        const std::vector<inference::BoundingBox>& detections,
        const std::vector<bool>& has_face_flags,
        const char* (*get_class_name)(int) = nullptr
    );
    
    // Get current active tracks
    const std::vector<Track>& getActiveTracks() const { return active_tracks_; }
    
    // Get track count
    size_t getTrackCount() const { return active_tracks_.size(); }
    
    // Reset all tracks
    void reset();
    
    // Configuration
    void setIoUThreshold(float threshold) { iou_threshold_ = threshold; }
    void setMaxLostFrames(int frames) { max_lost_frames_ = frames; }
    void setMinStableFrames(int frames) { min_stable_frames_ = frames; }

private:
    // Match detections to existing tracks
    std::vector<std::pair<int, int>> matchDetectionsToTracks(
        const std::vector<inference::BoundingBox>& detections
    );
    
    // Create new track
    Track createTrack(
        const inference::BoundingBox& detection,
        bool has_face,
        const char* (*get_class_name)(int)
    );
    
    // Update track with detection
    void updateTrack(
        Track& track,
        const inference::BoundingBox& detection,
        bool has_face
    );
    
    // Predict track position (simple linear prediction)
    cv::Rect predictTrackPosition(const Track& track);
    
    // Calculate IoU between rect and bbox
    float calculateIoU(const cv::Rect& rect, const inference::BoundingBox& bbox);

private:
    std::vector<Track> active_tracks_;
    int next_track_id_;
    
    // Configuration
    float iou_threshold_;        // IoU threshold for matching
    int max_lost_frames_;        // Max frames before removing track
    int min_stable_frames_;      // Min frames to consider track stable
};

// ============================================================================
// Kalman Filter Tracking (Optional Advanced Version)
// ============================================================================

class KalmanTracker {
public:
    KalmanTracker(const cv::Rect& initial_bbox);
    
    // Predict next position
    cv::Rect predict();
    
    // Update with measurement
    void update(const cv::Rect& measurement);
    
    // Get current position
    cv::Rect getPosition() const;
    
    // Get age
    int getAge() const { return age_; }
    
    // Get hit streak
    int getHitStreak() const { return hit_streak_; }
    
    // Get time since update
    int getTimeSinceUpdate() const { return time_since_update_; }
    
    // Mark as updated
    void markHit() {
        hit_streak_++;
        time_since_update_ = 0;
    }
    
    // Mark as missed
    void markMiss() {
        hit_streak_ = 0;
        time_since_update_++;
    }

private:
    cv::KalmanFilter kf_;
    int age_;
    int hit_streak_;
    int time_since_update_;
    cv::Rect last_bbox_;
};

} // namespace pipeline
