// inference/src/tracking.cpp
#include "inference/bounding_box.h"
#include "inference/tracking.h"
#include "inference/multi_model_infer.h"  // For FaceDetectionResult definition
#include <algorithm>
#include <cmath>
#include <iostream>

namespace inference {

// ============================================================================
// KALMAN BOX TRACKER IMPLEMENTATION
// ============================================================================

KalmanBoxTracker::KalmanBoxTracker(const BoundingBox& bbox, int track_id)
    : track_id_(track_id), age_(0), hit_streak_(0), time_since_update_(0) {
    
    // State: [cx, cy, s, r, vcx, vcy, vs]
    // Measurement: [cx, cy, s, r]
    kf_.init(7, 4, 0);
    
    // State transition matrix
    kf_.transitionMatrix = (cv::Mat_<float>(7, 7) <<
        1, 0, 0, 0, 1, 0, 0,  // cx = cx + vcx
        0, 1, 0, 0, 0, 1, 0,  // cy = cy + vcy
        0, 0, 1, 0, 0, 0, 1,  // s = s + vs
        0, 0, 0, 1, 0, 0, 0,  // r = r
        0, 0, 0, 0, 1, 0, 0,  // vcx = vcx
        0, 0, 0, 0, 0, 1, 0,  // vcy = vcy
        0, 0, 0, 0, 0, 0, 1   // vs = vs
    );
    
    // Measurement matrix
    kf_.measurementMatrix = cv::Mat::zeros(4, 7, CV_32F);
    kf_.measurementMatrix.at<float>(0, 0) = 1.0f;
    kf_.measurementMatrix.at<float>(1, 1) = 1.0f;
    kf_.measurementMatrix.at<float>(2, 2) = 1.0f;
    kf_.measurementMatrix.at<float>(3, 3) = 1.0f;
    
    // Process noise
    cv::setIdentity(kf_.processNoiseCov, cv::Scalar::all(0.01));
    kf_.processNoiseCov.at<float>(4, 4) = 0.01f;
    kf_.processNoiseCov.at<float>(5, 5) = 0.01f;
    kf_.processNoiseCov.at<float>(6, 6) = 0.01f;
    
    // Measurement noise
    cv::setIdentity(kf_.measurementNoiseCov, cv::Scalar::all(0.1));
    
    // Initial state covariance
    cv::setIdentity(kf_.errorCovPost, cv::Scalar::all(1.0));
    kf_.errorCovPost.at<float>(4, 4) = 10000.0f;
    kf_.errorCovPost.at<float>(5, 5) = 10000.0f;
    kf_.errorCovPost.at<float>(6, 6) = 10000.0f;
    
    // Initialize state
    cv::Mat init_state = convertBboxToMeasurement(bbox);
    kf_.statePost = cv::Mat::zeros(7, 1, CV_32F);
    init_state.copyTo(kf_.statePost(cv::Rect(0, 0, 1, 4)));
}

void KalmanBoxTracker::predict() {
    kf_.predict();
    age_++;
    time_since_update_++;
    if (time_since_update_ > 0) {
        hit_streak_ = 0;
    }
}

void KalmanBoxTracker::update(const BoundingBox& bbox) {
    cv::Mat measurement = convertBboxToMeasurement(bbox);
    kf_.correct(measurement);
    time_since_update_ = 0;
    hit_streak_++;
}

BoundingBox KalmanBoxTracker::getState() const {
    return measurementToBbox(kf_.statePost);
}

// FIXED: Added const qualifier
cv::Mat KalmanBoxTracker::convertBboxToMeasurement(const BoundingBox& bbox) const {
    float cx = (bbox.x1 + bbox.x2) / 2.0f;
    float cy = (bbox.y1 + bbox.y2) / 2.0f;
    float w = bbox.x2 - bbox.x1;
    float h = bbox.y2 - bbox.y1;
    float s = w * h;
    float r = w / std::max(h, 1e-6f);
    
    cv::Mat measurement = cv::Mat::zeros(4, 1, CV_32F);
    measurement.at<float>(0) = cx;
    measurement.at<float>(1) = cy;
    measurement.at<float>(2) = s;
    measurement.at<float>(3) = r;
    
    return measurement;
}

// FIXED: Added const qualifier
BoundingBox KalmanBoxTracker::measurementToBbox(const cv::Mat& state) const {
    float cx = state.at<float>(0);
    float cy = state.at<float>(1);
    float s = state.at<float>(2);
    float r = state.at<float>(3);
    
    float h = std::sqrt(s / std::max(r, 1e-6f));
    float w = r * h;
    
    BoundingBox bbox;
    bbox.x1 = cx - w / 2.0f;
    bbox.y1 = cy - h / 2.0f;
    bbox.x2 = cx + w / 2.0f;
    bbox.y2 = cy + h / 2.0f;
    bbox.score = 1.0f;
    bbox.class_id = 0;
    
    return bbox;
}

// ============================================================================
// OBJECT TRACKER IMPLEMENTATION
// ============================================================================

ObjectTracker::ObjectTracker(const TrackerConfig& config)
    : config_(config), next_track_id_(1) {
}

ObjectTracker::~ObjectTracker() = default;

std::vector<TrackedObject> ObjectTracker::update(
    const std::vector<BoundingBox>& detections,
    const std::vector<std::vector<float>>& features) {
    
    // Predict all existing tracks
    for (auto& tracker : trackers_) {
        tracker->predict();
    }
    
    // Associate detections to trackers
    auto matches = associateDetectionsToTrackers(detections, features);
    
    // Track which detections and trackers are matched
    std::vector<bool> det_matched(detections.size(), false);
    std::vector<bool> trk_matched(trackers_.size(), false);
    
    // Update matched tracks
    for (const auto& match : matches) {
        updateTrack(match.tracker_idx, detections[match.detection_idx],
                   match.detection_idx, features);
        det_matched[match.detection_idx] = true;
        trk_matched[match.tracker_idx] = true;
    }
    
    // Create new tracks for unmatched detections
    for (size_t i = 0; i < detections.size(); i++) {
        if (!det_matched[i]) {
            initNewTrack(detections[i], i, features);
        }
    }
    
    // Mark unmatched trackers as lost
    // BUG-AIW-OBJTRACK-AV-01 (debug 2026-05-10): pre-fix this loop was bounded
    // by `trackers_.size()`, but `trackers_` may have grown above the size
    // that was used to construct `trk_matched` — `initNewTrack` at line 157
    // appends new trackers for unmatched detections. On the very first frame
    // with any detection, `trackers_` starts empty, so `trk_matched` is built
    // as `vector<bool>(0, false)` whose `_M_start` is NULL on MSVC. After
    // `initNewTrack` runs, `trackers_.size()` jumps to N > 0, then reading
    // `trk_matched[0]` dereferences NULL → ACCESS_VIOLATION 0xC0000005
    // in the ai_worker_v2 process. Reproduced deterministically on cam 2 and
    // cam 3 (7 minidumps all crashing at exe offset 0x36BC4 = bit-iter over
    // vector<bool>). Fix: iterate over the ORIGINAL trk_matched range — the
    // freshly created tracks at indices ≥ trk_matched.size() were built from
    // detections in initNewTrack and are matched-by-definition, never lost.
    std::vector<int> unmatched_trackers;
    for (size_t i = 0; i < trk_matched.size(); i++) {
        if (!trk_matched[i]) {
            unmatched_trackers.push_back(i);
        }
    }
    markLostTracks(unmatched_trackers);
    
    // Remove stale tracks
    removeStaleTracks();
    
    // Return active tracks
    return getActiveTracks();
}

std::vector<ObjectTracker::Match> ObjectTracker::associateDetectionsToTrackers(
    const std::vector<BoundingBox>& detections,
    const std::vector<std::vector<float>>& features) {
    
    if (trackers_.empty() || detections.empty()) {
        return {};
    }
    
    // Build cost matrix (IOU + feature similarity if available)
    std::vector<std::vector<float>> cost_matrix(
        trackers_.size(), std::vector<float>(detections.size(), 0.0f)
    );
    
    for (size_t t = 0; t < trackers_.size(); t++) {
        BoundingBox pred_bbox = trackers_[t]->getState();
        
        for (size_t d = 0; d < detections.size(); d++) {
            float iou = computeIOU(pred_bbox, detections[d]);
            float cost = iou;
            
            // Add feature similarity if available
            if (config_.use_reid && !features.empty() && d < features.size()) {
                auto track_it = tracks_.find(trackers_[t]->getTrackId());
                if (track_it != tracks_.end() && !track_it->second.feature.empty()) {
                    float feat_sim = computeFeatureSimilarity(
                        track_it->second.feature, features[d]
                    );
                    cost = 0.5f * iou + 0.5f * feat_sim;
                }
            }
            
            cost_matrix[t][d] = cost;
        }
    }
    
    // Hungarian matching (greedy approximation)
    std::vector<Match> matches;
    std::vector<bool> det_used(detections.size(), false);
    std::vector<bool> trk_used(trackers_.size(), false);
    
    // Greedy matching: pick highest cost matches first
    while (true) {
        float max_cost = config_.iou_threshold;
        int best_t = -1, best_d = -1;
        
        for (size_t t = 0; t < trackers_.size(); t++) {
            if (trk_used[t]) continue;
            for (size_t d = 0; d < detections.size(); d++) {
                if (det_used[d]) continue;
                if (cost_matrix[t][d] > max_cost) {
                    max_cost = cost_matrix[t][d];
                    best_t = t;
                    best_d = d;
                }
            }
        }
        
        if (best_t == -1) break;
        
        Match m;
        m.detection_idx = best_d;
        m.tracker_idx = best_t;
        m.iou = max_cost;
        matches.push_back(m);
        
        det_used[best_d] = true;
        trk_used[best_t] = true;
    }
    
    return matches;
}

float ObjectTracker::computeIOU(const BoundingBox& a, const BoundingBox& b) {
    float inter_x1 = std::max(a.x1, b.x1);
    float inter_y1 = std::max(a.y1, b.y1);
    float inter_x2 = std::min(a.x2, b.x2);
    float inter_y2 = std::min(a.y2, b.y2);
    
    float inter_w = std::max(0.0f, inter_x2 - inter_x1);
    float inter_h = std::max(0.0f, inter_y2 - inter_y1);
    float inter_area = inter_w * inter_h;
    
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float union_area = area_a + area_b - inter_area;
    
    return (union_area > 0) ? (inter_area / union_area) : 0.0f;
}

float ObjectTracker::computeFeatureSimilarity(
    const std::vector<float>& feat1,
    const std::vector<float>& feat2) {
    
    if (feat1.empty() || feat2.empty() || feat1.size() != feat2.size()) {
        return 0.0f;
    }
    
    float dot = 0.0f, norm1 = 0.0f, norm2 = 0.0f;
    for (size_t i = 0; i < feat1.size(); i++) {
        dot += feat1[i] * feat2[i];
        norm1 += feat1[i] * feat1[i];
        norm2 += feat2[i] * feat2[i];
    }
    
    float denom = std::sqrt(norm1) * std::sqrt(norm2);
    return (denom > 0) ? (dot / denom) : 0.0f;
}

void ObjectTracker::initNewTrack(const BoundingBox& det, int det_idx,
                                const std::vector<std::vector<float>>& features) {
    int track_id = next_track_id_++;
    
    auto tracker = std::make_shared<KalmanBoxTracker>(det, track_id);
    trackers_.push_back(tracker);
    
    TrackedObject track;
    track.track_id = track_id;
    track.bbox = det;
    track.state = TrackState::NEW;
    track.age = 0;
    track.hits = 1;
    track.time_since_update = 0;
    track.confidence = det.score;
    track.class_id = det.class_id;
    track.label = det.label;
    track.updateHistory(det);
    
    if (config_.use_reid && !features.empty() && det_idx < features.size()) {
        track.feature = features[det_idx];
    }
    
    tracks_[track_id] = track;
}

void ObjectTracker::updateTrack(int tracker_idx, const BoundingBox& det,
                               int det_idx,
                               const std::vector<std::vector<float>>& features) {
    auto& tracker = trackers_[tracker_idx];
    tracker->update(det);
    
    int track_id = tracker->getTrackId();
    auto& track = tracks_[track_id];
    
    track.bbox = det;
    track.age++;
    track.hits++;
    track.time_since_update = 0;
    track.confidence = det.score;
    track.updateHistory(det);
    
    // Update state
    if (track.hits >= config_.min_hits) {
        track.state = TrackState::TRACKED;
    }
    
    // Update feature if available
    if (config_.use_reid && !features.empty() && det_idx < features.size()) {
        // EMA update
        const float alpha = 0.7f;
        if (track.feature.empty()) {
            track.feature = features[det_idx];
        } else {
            for (size_t i = 0; i < track.feature.size() && i < features[det_idx].size(); i++) {
                track.feature[i] = alpha * track.feature[i] + (1 - alpha) * features[det_idx][i];
            }
        }
    }
}

void ObjectTracker::markLostTracks(const std::vector<int>& unmatched_tracker_indices) {
    for (int idx : unmatched_tracker_indices) {
        int track_id = trackers_[idx]->getTrackId();
        auto& track = tracks_[track_id];
        
        track.time_since_update++;
        
        if (track.time_since_update > 1) {
            track.state = TrackState::LOST;
        }
    }
}

void ObjectTracker::removeStaleTracks() {
    std::vector<int> to_remove;
    
    for (size_t i = 0; i < trackers_.size(); i++) {
        int track_id = trackers_[i]->getTrackId();
        auto& track = tracks_[track_id];
        
        // Remove if lost for too long or never confirmed
        bool should_remove = false;
        
        if (track.time_since_update > config_.max_age) {
            should_remove = true;
        }
        
        if (track.state == TrackState::NEW && track.age > config_.min_hits) {
            should_remove = true;
        }
        
        if (should_remove) {
            track.state = TrackState::REMOVED;
            to_remove.push_back(i);
        }
    }
    
    // Remove trackers in reverse order
    std::sort(to_remove.rbegin(), to_remove.rend());
    for (int idx : to_remove) {
        int track_id = trackers_[idx]->getTrackId();
        tracks_.erase(track_id);
        trackers_.erase(trackers_.begin() + idx);
    }

    // SAFETY CAP: Prevent unbounded growth (Memory Leak Protection)
    const size_t MAX_TRACKS_LIMIT = 2000;
    if (trackers_.size() > MAX_TRACKS_LIMIT) {
        std::cerr << "[ObjectTracker] WARNING: Track limit reached (" << trackers_.size() 
                  << "), removing oldest tracks.\n";
        
        // Remove oldest 10%
        size_t remove_count = trackers_.size() / 10;
        
        if (remove_count > 0 && remove_count <= trackers_.size()) {
            for (size_t i = 0; i < remove_count; i++) {
                int track_id = trackers_[i]->getTrackId();
                tracks_.erase(track_id);
            }
            trackers_.erase(trackers_.begin(), trackers_.begin() + remove_count);
        }
    }
}

std::vector<TrackedObject> ObjectTracker::getActiveTracks() const {
    std::vector<TrackedObject> active;
    
    for (const auto& [id, track] : tracks_) {
        if (track.state == TrackState::TRACKED || 
            (track.state == TrackState::NEW && track.hits >= config_.min_hits)) {
            active.push_back(track);
        }
    }
    
    return active;
}

TrackedObject* ObjectTracker::getTrack(int track_id) {
    auto it = tracks_.find(track_id);
    return (it != tracks_.end()) ? &it->second : nullptr;
}

void ObjectTracker::reset() {
    trackers_.clear();
    tracks_.clear();
    next_track_id_ = 1;
}

ObjectTracker::Stats ObjectTracker::getStats() const {
    Stats stats{};
    stats.total_tracks = tracks_.size();
    
    for (const auto& [id, track] : tracks_) {
        switch (track.state) {
            case TrackState::TRACKED:
                stats.active_tracks++;
                break;
            case TrackState::LOST:
                stats.lost_tracks++;
                break;
            case TrackState::REMOVED:
                stats.removed_tracks++;
                break;
            default:
                break;
        }
    }
    
    return stats;
}

// ============================================================================
// FACE TRACKER IMPLEMENTATION
// ============================================================================

FaceTracker::FaceTracker(float iou_threshold, int max_age, int min_hits)
    : iou_threshold_(iou_threshold)
    , max_age_(max_age)
    , min_hits_(min_hits)
    , next_track_id_(1) {
}

std::vector<TrackedFace> FaceTracker::update(
    const std::vector<FaceDetectionResult>& face_detections) {
    
    // Match detections to existing tracks
    std::vector<bool> det_matched(face_detections.size(), false);
    std::vector<int> track_ids;
    
    for (auto& [id, track] : tracks_) {
        track_ids.push_back(id);
    }
    
    std::vector<bool> trk_matched(track_ids.size(), false);
    
    // Greedy matching
    for (size_t d = 0; d < face_detections.size(); d++) {
        float best_iou = iou_threshold_;
        int best_track_idx = -1;
        
        for (size_t t = 0; t < track_ids.size(); t++) {
            if (trk_matched[t]) continue;
            
            auto& track = tracks_[track_ids[t]];
            float iou = computeFaceIOU(track, face_detections[d]);
            
            // Also consider embedding similarity
            float emb_sim = computeEmbeddingSimilarity(
                track.embedding, face_detections[d].embedding, 512
            );
            
            float score = 0.6f * iou + 0.4f * emb_sim;
            
            if (score > best_iou) {
                best_iou = score;
                best_track_idx = t;
            }
        }
        
        if (best_track_idx >= 0) {
            int track_id = track_ids[best_track_idx];
            auto& track = tracks_[track_id];
            
            // Update track
            const auto& det = face_detections[d];
            // EMA Smoothing for coordinates
            float alpha = 0.7f; // Smoothing factor (0.7 = 70% history, 30% new)
            track.x1 = alpha * track.x1 + (1.0f - alpha) * det.x1;
            track.y1 = alpha * track.y1 + (1.0f - alpha) * det.y1;
            track.x2 = alpha * track.x2 + (1.0f - alpha) * det.x2;
            track.y2 = alpha * track.y2 + (1.0f - alpha) * det.y2;
            
            track.confidence = det.confidence;
            track.person_id = det.person_id;
            // Update identity votes (Cumulative scoring)
            if (det.person_id != -1) {
                track.identity_scores[det.person_id] += det.similarity;
                track.identity_names[det.person_id] = det.name;
            } else {
                // Unknown/Background vote
                track.identity_scores[-1] += 0.5f; // Constant penalty for uncertainty
            }
            
            // Re-evaluate stable identity (Winner takes all based on cumulative score)
            int best_id = -1;
            float max_score = -1.0f;
            for (const auto& [id, score] : track.identity_scores) {
                if (score > max_score) {
                    max_score = score;
                    best_id = id;
                }
            }
            
            if (best_id != -1) {
                track.stable_person_id = best_id;
                track.stable_name = track.identity_names[best_id];
            } else {
                track.stable_person_id = -1;
                track.stable_name = "Unknown";
            }
            
            // For backward compatibility/reporting, update latest
            track.person_id = track.stable_person_id;
            track.name = track.stable_name;
            
            track.hits++;
            track.age = 0;
            track.state = TrackState::TRACKED;
            
            // Update embedding (EMA)
            if (!track.embedding.empty()) {
                for (int i = 0; i < 512; i++) {
                    track.embedding[i] = 0.8f * track.embedding[i] + 
                                        0.2f * det.embedding[i];
                }
            } else {
                track.embedding.assign(det.embedding, det.embedding + 512);
            }
            
            det_matched[d] = true;
            trk_matched[best_track_idx] = true;
        }
    }
    
    // Create new tracks for unmatched detections
    for (size_t d = 0; d < face_detections.size(); d++) {
        if (!det_matched[d]) {
            TrackedFace new_track;
            new_track.track_id = next_track_id_++;
            new_track.x1 = face_detections[d].x1;
            new_track.y1 = face_detections[d].y1;
            new_track.x2 = face_detections[d].x2;
            new_track.y2 = face_detections[d].y2;
            new_track.confidence = face_detections[d].confidence;
            new_track.person_id = face_detections[d].person_id;
            new_track.name = face_detections[d].name;
            
            // Initialize identity voting
            if (new_track.person_id != -1) {
                new_track.identity_scores[new_track.person_id] = face_detections[d].similarity;
                new_track.identity_names[new_track.person_id] = new_track.name;
                new_track.stable_person_id = new_track.person_id;
                new_track.stable_name = new_track.name;
            } else {
                new_track.identity_scores[-1] = 0.5f;
                new_track.stable_person_id = -1;
                new_track.stable_name = "Face"; // Initial generic label
            }
            
            new_track.hits = 1;
            new_track.age = 0;
            new_track.state = TrackState::NEW;
            new_track.embedding.assign(
                face_detections[d].embedding,
                face_detections[d].embedding + 512
            );
            
            tracks_[new_track.track_id] = new_track;
        }
    }
    
    // Age unmatched tracks
    std::vector<int> to_remove;
    for (size_t t = 0; t < track_ids.size(); t++) {
        if (!trk_matched[t]) {
            int track_id = track_ids[t];
            auto& track = tracks_[track_id];
            track.age++;
            
            if (track.age > max_age_) {
                to_remove.push_back(track_id);
            } else {
                track.state = TrackState::LOST;
            }
        }
    }
    
    // Remove stale tracks
    for (int id : to_remove) {
        tracks_.erase(id);
    }
    
    // Return active tracks
    std::vector<TrackedFace> active;
    for (auto& [id, track] : tracks_) {
        if (track.hits >= min_hits_) {
            // Final sync for output
            track.person_id = track.stable_person_id;
            track.name = track.stable_name;
            active.push_back(track);
        }
    }
    
    return active;
}

// FIXED: Added const qualifier
float FaceTracker::computeFaceIOU(const TrackedFace& a, const FaceDetectionResult& b) const {
    float inter_x1 = std::max(a.x1, b.x1);
    float inter_y1 = std::max(a.y1, b.y1);
    float inter_x2 = std::min(a.x2, b.x2);
    float inter_y2 = std::min(a.y2, b.y2);
    
    float inter_w = std::max(0.0f, inter_x2 - inter_x1);
    float inter_h = std::max(0.0f, inter_y2 - inter_y1);
    float inter_area = inter_w * inter_h;
    
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float union_area = area_a + area_b - inter_area;
    
    return (union_area > 0) ? (inter_area / union_area) : 0.0f;
}

// FIXED: Added const qualifier
float FaceTracker::computeEmbeddingSimilarity(
    const std::vector<float>& a, const float* b, int dim) const {
    
    if (a.empty() || !b) return 0.0f;
    
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < dim && i < static_cast<int>(a.size()); i++) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    
    float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
    return (denom > 0) ? (dot / denom) : 0.0f;
}

void FaceTracker::reset() {
    tracks_.clear();
    next_track_id_ = 1;
}

int FaceTracker::getActiveTrackCount() const {
    int count = 0;
    for (const auto& [id, track] : tracks_) {
        if (track.hits >= min_hits_) {
            count++;
        }
    }
    return count;
}

} // namespace inference