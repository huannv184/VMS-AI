// inference/include/inference/tracking.h
#pragma once

#include "inference/bounding_box.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <memory>
#include <map>
#include <deque>
#include <cstring>

namespace inference {

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

// Forward declare FaceDetectionResult (defined in multi_model_infer.h)
struct FaceDetectionResult;

// ============================================================================
// TRACK STATE
// ============================================================================

enum class TrackState {
    NEW,         // Newly created track
    TRACKED,     // Successfully tracked
    LOST,        // Lost for a few frames
    REMOVED      // Removed from tracking
};

// ============================================================================
// KALMAN FILTER FOR TRACKING
// ============================================================================

class KalmanBoxTracker {
public:
    KalmanBoxTracker(const BBox& bbox, int track_id);
    
    void predict();
    void update(const BBox& bbox);
    BBox getState() const;
    
    int getTrackId() const { return track_id_; }
    int getAge() const { return age_; }
    int getHitStreak() const { return hit_streak_; }
    int getTimeSinceLost() const { return time_since_update_; }
    
private:
    cv::KalmanFilter kf_;
    int track_id_;
    int age_;
    int hit_streak_;
    int time_since_update_;
    
    // FIXED: Added const qualifier
    cv::Mat convertBboxToMeasurement(const BBox& bbox) const;
    BBox measurementToBbox(const cv::Mat& state) const;
};

// ============================================================================
// TRACKED OBJECT
// ============================================================================

struct TrackedObject {
    int track_id;
    BBox bbox;
    TrackState state;
    int age;                    // Total frames since creation
    int hits;                   // Consecutive successful matches
    int time_since_update;      // Frames since last update
    float confidence;
    int class_id;
    std::string label;
    
    // Motion history for smoothing
    std::deque<BBox> history;
    static constexpr size_t MAX_HISTORY = 10;
    
    // Feature vector for ReID (optional)
    std::vector<float> feature;
    
    TrackedObject() 
        : track_id(-1), state(TrackState::NEW), age(0)
        , hits(0), time_since_update(0), confidence(0.0f)
        , class_id(-1) {}
    
    void updateHistory(const BBox& new_bbox) {
        history.push_back(new_bbox);
        if (history.size() > MAX_HISTORY) {
            history.pop_front();
        }
    }
    
    BBox getSmoothedBbox() const {
        if (history.empty()) return bbox;
        
        float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        for (const auto& b : history) {
            x1 += b.x1;
            y1 += b.y1;
            x2 += b.x2;
            y2 += b.y2;
        }
        
        size_t n = history.size();
        BBox smoothed = bbox;
        smoothed.x1 = x1 / n;
        smoothed.y1 = y1 / n;
        smoothed.x2 = x2 / n;
        smoothed.y2 = y2 / n;
        
        return smoothed;
    }
};

// ============================================================================
// TRACKER CONFIGURATION
// ============================================================================

struct TrackerConfig {
    float iou_threshold = 0.3f;           // IOU threshold for matching
    int max_age = 30;                     // Max frames to keep lost tracks
    int min_hits = 3;                     // Min hits before confirming track
    float feature_similarity_threshold = 0.6f;  // For ReID matching
    bool use_reid = false;                // Enable ReID features
    
    // Motion model parameters
    float process_noise = 0.01f;          // Process noise for Kalman filter
    float measurement_noise = 0.1f;       // Measurement noise
};

// ============================================================================
// OBJECT TRACKER (DeepSORT-like)
// ============================================================================

class ObjectTracker {
public:
    explicit ObjectTracker(const TrackerConfig& config = TrackerConfig());
    ~ObjectTracker();
    
    // Main tracking function
    std::vector<TrackedObject> update(
        const std::vector<BBox>& detections,
        const std::vector<std::vector<float>>& features = {}
    );
    
    // Get all active tracks
    std::vector<TrackedObject> getActiveTracks() const;
    
    // Get specific track by ID
    TrackedObject* getTrack(int track_id);
    
    // Clear all tracks
    void reset();
    
    // Get tracking statistics
    struct Stats {
        int total_tracks;
        int active_tracks;
        int lost_tracks;
        int removed_tracks;
    };
    Stats getStats() const;
    
private:
    TrackerConfig config_;
    std::vector<std::shared_ptr<KalmanBoxTracker>> trackers_;
    std::map<int, TrackedObject> tracks_;
    int next_track_id_;
    
    // Matching algorithms
    struct Match {
        int detection_idx;
        int tracker_idx;
        float iou;
        float feature_sim;
    };
    
    std::vector<Match> associateDetectionsToTrackers(
        const std::vector<BBox>& detections,
        const std::vector<std::vector<float>>& features
    );
    
    float computeIOU(const BBox& a, const BBox& b);
    float computeFeatureSimilarity(
        const std::vector<float>& feat1,
        const std::vector<float>& feat2
    );
    
    void initNewTrack(const BBox& det, int det_idx,
                     const std::vector<std::vector<float>>& features);
    void updateTrack(int tracker_idx, const BBox& det, int det_idx,
                    const std::vector<std::vector<float>>& features);
    void markLostTracks(const std::vector<int>& unmatched_tracker_indices);
    void removeStaleTracks();
};

// ============================================================================
// FACE TRACKER
// ============================================================================

struct TrackedFace {
    int track_id;
    float x1, y1, x2, y2;
    float confidence;
    int person_id;
    std::string name;
    int age;
    int hits;
    TrackState state;
    
    std::deque<cv::Rect2f> history;
    std::vector<float> embedding;
    
    // Identity stabilization
    std::map<int, float> identity_scores; // person_id -> cumulative similarity
    std::map<int, std::string> identity_names; // person_id -> name
    int stable_person_id;
    std::string stable_name;
    
    TrackedFace() 
        : track_id(-1), x1(0), y1(0), x2(0), y2(0)
        , confidence(0), person_id(-1), age(0)
        , hits(0), state(TrackState::NEW)
        , stable_person_id(-1), stable_name("Face") {}
};

class FaceTracker {
public:
    explicit FaceTracker(float iou_threshold = 0.3f, int max_age = 30, int min_hits = 3);
    
    std::vector<TrackedFace> update(
        const std::vector<FaceDetectionResult>& face_detections
    );
    
    void reset();
    int getActiveTrackCount() const;
    
private:
    float iou_threshold_;
    int max_age_;
    int min_hits_;
    int next_track_id_;
    
    std::map<int, TrackedFace> tracks_;
    
    // FIXED: Added const qualifier
    float computeFaceIOU(const TrackedFace& a, const FaceDetectionResult& b) const;
    float computeEmbeddingSimilarity(const std::vector<float>& a, const float* b, int dim) const;
};

} // namespace inference