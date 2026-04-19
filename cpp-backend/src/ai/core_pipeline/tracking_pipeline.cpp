#include "tracking_pipeline.h"
#include <algorithm>
#include <iostream>

namespace pipeline {

// ============================================================================
// TrackingPipeline Implementation
// ============================================================================

TrackingPipeline::TrackingPipeline(
    float iou_threshold,
    int max_lost_frames,
    int min_stable_frames)
    : next_track_id_(1)
    , iou_threshold_(iou_threshold)
    , max_lost_frames_(max_lost_frames)
    , min_stable_frames_(min_stable_frames)
{
}

std::vector<Track> TrackingPipeline::update(
    const std::vector<inference::BoundingBox>& detections,
    const std::vector<bool>& has_face_flags,
    const char* (*get_class_name)(int))
{
    // Match detections to existing tracks
    auto matches = matchDetectionsToTracks(detections);
    
    // Create set of matched detection indices
    std::set<int> matched_detection_indices;
    std::set<int> matched_track_indices;
    
    for (const auto& [track_idx, det_idx] : matches) {
        matched_track_indices.insert(track_idx);
        matched_detection_indices.insert(det_idx);
    }
    
    // Update matched tracks
    for (const auto& [track_idx, det_idx] : matches) {
        bool has_face = det_idx < has_face_flags.size() ? has_face_flags[det_idx] : false;
        updateTrack(active_tracks_[track_idx], detections[det_idx], has_face);
    }
    
    // Create new tracks for unmatched detections
    for (size_t i = 0; i < detections.size(); i++) {
        if (matched_detection_indices.find(i) == matched_detection_indices.end()) {
            bool has_face = i < has_face_flags.size() ? has_face_flags[i] : false;
            Track new_track = createTrack(detections[i], has_face, get_class_name);
            active_tracks_.push_back(new_track);
        }
    }
    
    // Update unmatched tracks (mark as lost)
    std::vector<Track> updated_tracks;
    for (size_t i = 0; i < active_tracks_.size(); i++) {
        auto& track = active_tracks_[i];
        
        if (matched_track_indices.find(i) == matched_track_indices.end()) {
            track.lost_frames++;
            track.stable_frames = 0;
        }
        
        // Keep track if not lost for too long
        if (track.lost_frames < max_lost_frames_) {
            track.age++;
            updated_tracks.push_back(track);
        }
    }
    
    active_tracks_ = std::move(updated_tracks);
    
    return active_tracks_;
}

std::vector<std::pair<int, int>> TrackingPipeline::matchDetectionsToTracks(
    const std::vector<inference::BoundingBox>& detections)
{
    std::vector<std::pair<int, int>> matches;
    
    if (active_tracks_.empty() || detections.empty()) {
        return matches;
    }
    
    // Build cost matrix (1 - IoU)
    std::vector<std::vector<float>> cost_matrix(
        active_tracks_.size(),
        std::vector<float>(detections.size(), 1.0f)
    );
    
    for (size_t i = 0; i < active_tracks_.size(); i++) {
        cv::Rect predicted = predictTrackPosition(active_tracks_[i]);
        
        for (size_t j = 0; j < detections.size(); j++) {
            float iou = calculateIoU(predicted, detections[j]);
            cost_matrix[i][j] = 1.0f - iou;
        }
    }
    
    // Simple greedy matching
    std::set<int> matched_tracks;
    std::set<int> matched_detections;
    
    while (matched_tracks.size() < active_tracks_.size() && 
           matched_detections.size() < detections.size()) {
        
        float min_cost = 1.0f;
        int best_track = -1;
        int best_detection = -1;
        
        for (size_t i = 0; i < active_tracks_.size(); i++) {
            if (matched_tracks.find(i) != matched_tracks.end()) continue;
            
            for (size_t j = 0; j < detections.size(); j++) {
                if (matched_detections.find(j) != matched_detections.end()) continue;
                
                if (cost_matrix[i][j] < min_cost) {
                    min_cost = cost_matrix[i][j];
                    best_track = i;
                    best_detection = j;
                }
            }
        }
        
        if (best_track >= 0 && min_cost < (1.0f - iou_threshold_)) {
            matches.push_back({best_track, best_detection});
            matched_tracks.insert(best_track);
            matched_detections.insert(best_detection);
        } else {
            break;
        }
    }
    
    return matches;
}

Track TrackingPipeline::createTrack(
    const inference::BoundingBox& detection,
    bool has_face,
    const char* (*get_class_name)(int))
{
    Track track;
    track.track_id = next_track_id_++;
    track.bbox = cv::Rect(
        static_cast<int>(detection.x1),
        static_cast<int>(detection.y1),
        static_cast<int>(detection.x2 - detection.x1),
        static_cast<int>(detection.y2 - detection.y1)
    );
    track.confidence = detection.score;
    track.class_id = detection.class_id;
    track.class_name = get_class_name ? get_class_name(detection.class_id) : "object";
    track.has_face = has_face;
    track.age = 0;
    track.stable_frames = 1;
    track.lost_frames = 0;
    track.velocity = cv::Point2f(0, 0);
    
    return track;
}

void TrackingPipeline::updateTrack(
    Track& track,
    const inference::BoundingBox& detection,
    bool has_face)
{
    // Calculate velocity (simple)
    cv::Rect new_bbox(
        static_cast<int>(detection.x1),
        static_cast<int>(detection.y1),
        static_cast<int>(detection.x2 - detection.x1),
        static_cast<int>(detection.y2 - detection.y1)
    );
    
    track.velocity.x = new_bbox.x - track.bbox.x;
    track.velocity.y = new_bbox.y - track.bbox.y;
    
    // Smooth update (EMA)
    float alpha = 0.7f;
    track.bbox.x = static_cast<int>(alpha * new_bbox.x + (1 - alpha) * track.bbox.x);
    track.bbox.y = static_cast<int>(alpha * new_bbox.y + (1 - alpha) * track.bbox.y);
    track.bbox.width = static_cast<int>(alpha * new_bbox.width + (1 - alpha) * track.bbox.width);
    track.bbox.height = static_cast<int>(alpha * new_bbox.height + (1 - alpha) * track.bbox.height);
    
    track.confidence = alpha * detection.score + (1 - alpha) * track.confidence;
    track.has_face = has_face;
    track.stable_frames++;
    track.lost_frames = 0;
}

cv::Rect TrackingPipeline::predictTrackPosition(const Track& track) {
    // Simple linear prediction
    cv::Rect predicted = track.bbox;
    predicted.x += static_cast<int>(track.velocity.x);
    predicted.y += static_cast<int>(track.velocity.y);
    return predicted;
}

float TrackingPipeline::calculateIoU(
    const cv::Rect& rect,
    const inference::BoundingBox& bbox)
{
    float x1 = std::max(static_cast<float>(rect.x), bbox.x1);
    float y1 = std::max(static_cast<float>(rect.y), bbox.y1);
    float x2 = std::min(static_cast<float>(rect.x + rect.width), bbox.x2);
    float y2 = std::min(static_cast<float>(rect.y + rect.height), bbox.y2);
    
    if (x2 <= x1 || y2 <= y1) {
        return 0.0f;
    }
    
    float intersection = (x2 - x1) * (y2 - y1);
    float rect_area = rect.width * rect.height;
    float bbox_area = (bbox.x2 - bbox.x1) * (bbox.y2 - bbox.y1);
    float union_area = rect_area + bbox_area - intersection;
    
    return union_area > 0 ? intersection / union_area : 0.0f;
}

void TrackingPipeline::reset() {
    active_tracks_.clear();
    next_track_id_ = 1;
}

// ============================================================================
// KalmanTracker Implementation
// ============================================================================

KalmanTracker::KalmanTracker(const cv::Rect& initial_bbox)
    : age_(0)
    , hit_streak_(0)
    , time_since_update_(0)
    , last_bbox_(initial_bbox)
{
    // State: [x, y, width, height, dx, dy, dw, dh]
    // Measurement: [x, y, width, height]
    kf_.init(8, 4, 0);
    
    // Transition matrix
    kf_.transitionMatrix = (cv::Mat_<float>(8, 8) <<
        1, 0, 0, 0, 1, 0, 0, 0,
        0, 1, 0, 0, 0, 1, 0, 0,
        0, 0, 1, 0, 0, 0, 1, 0,
        0, 0, 0, 1, 0, 0, 0, 1,
        0, 0, 0, 0, 1, 0, 0, 0,
        0, 0, 0, 0, 0, 1, 0, 0,
        0, 0, 0, 0, 0, 0, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 1
    );
    
    // Measurement matrix
    kf_.measurementMatrix = cv::Mat::zeros(4, 8, CV_32F);
    kf_.measurementMatrix.at<float>(0, 0) = 1;
    kf_.measurementMatrix.at<float>(1, 1) = 1;
    kf_.measurementMatrix.at<float>(2, 2) = 1;
    kf_.measurementMatrix.at<float>(3, 3) = 1;
    
    // Initialize state
    kf_.statePost.at<float>(0) = initial_bbox.x;
    kf_.statePost.at<float>(1) = initial_bbox.y;
    kf_.statePost.at<float>(2) = initial_bbox.width;
    kf_.statePost.at<float>(3) = initial_bbox.height;
    
    // Process noise
    cv::setIdentity(kf_.processNoiseCov, cv::Scalar::all(1e-2));
    
    // Measurement noise
    cv::setIdentity(kf_.measurementNoiseCov, cv::Scalar::all(1e-1));
    
    // Error covariance
    cv::setIdentity(kf_.errorCovPost, cv::Scalar::all(1));
}

cv::Rect KalmanTracker::predict() {
    cv::Mat prediction = kf_.predict();
    
    age_++;
    time_since_update_++;
    
    last_bbox_.x = static_cast<int>(prediction.at<float>(0));
    last_bbox_.y = static_cast<int>(prediction.at<float>(1));
    last_bbox_.width = static_cast<int>(prediction.at<float>(2));
    last_bbox_.height = static_cast<int>(prediction.at<float>(3));
    
    return last_bbox_;
}

void KalmanTracker::update(const cv::Rect& measurement) {
    cv::Mat meas = (cv::Mat_<float>(4, 1) <<
        measurement.x,
        measurement.y,
        measurement.width,
        measurement.height
    );
    
    kf_.correct(meas);
    
    last_bbox_ = measurement;
    markHit();
}

cv::Rect KalmanTracker::getPosition() const {
    return last_bbox_;
}

} // namespace pipeline
