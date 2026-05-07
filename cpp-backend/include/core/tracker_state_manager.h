// ==============================================================
// File: include/core/tracker_state_manager.h
// Per-camera lightweight tracker. Maintains a stable virtual_track_id
// across frames using greedy IoU matching, so downstream modules
// (PeopleCountTracker — line crossing & zone enter/exit) have a
// reliable identity even when ai_worker runs with bypass_tracker=ON
// (track_id=-1 from the worker side).
//
// Cost: O(N×M) per camera per frame, N,M ≤ ~30 detections → < 1ms.
// Thread safety: per-camera mutex; shared rwlock only for camera-map mutation.
// ==============================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>

namespace vms::core {

// ── Detection input (one per object in a frame) ────────────────────────────
struct DetectionInput {
    int          class_id      = -1;
    std::string  label;                  // "person", "Face", ...
    cv::Rect2f   bbox;                   // pixel coords [x, y, w, h]
    float        confidence    = 0.0f;
    int          worker_track_id = -1;   // -1 nếu bypass_tracker=ON
    int          person_id     = -1;     // -1 nếu chưa face match

    // Build from one element of metadata["objects"]
    static DetectionInput fromJson(const nlohmann::json& obj);
};

// ── Tracked object state (returned to caller) ──────────────────────────────
struct TrackState {
    int          virtual_track_id = 0;   // backend-assigned, stable across frames
    cv::Point2f  centroid_curr;          // bbox centroid this frame
    cv::Point2f  centroid_prev;          // centroid previous frame (==curr on first sight)
    cv::Rect2f   bbox_curr;
    int          last_zone_id = -1;      // zone id from previous frame (for enter/exit)
    int          curr_zone_id = -1;      // zone id this frame
    int          person_id    = -1;
    int          worker_track_id = -1;
    std::string  object_class;
    int64_t      first_seen_ms = 0;
    int64_t      last_seen_ms  = 0;
    bool         is_new        = false;  // true on first frame this track appeared
};

// ── Manager (singleton) ────────────────────────────────────────────────────
class TrackerStateManager {
public:
    static TrackerStateManager& getInstance();

    // Match detections against existing tracks (per camera). Returns the
    // updated tracks for this frame (only those that matched a detection).
    // Tracks unmatched in 5s are GC'd internally.
    std::vector<TrackState> updateFrame(int camera_id,
                                        const std::vector<DetectionInput>& dets,
                                        int64_t ts_ms);

    // Drop all tracks for a camera (call on camera disable/delete).
    void resetCamera(int camera_id);

    // For debugging / health endpoint.
    std::size_t trackCount(int camera_id);

private:
    TrackerStateManager() = default;
    ~TrackerStateManager() = default;
    TrackerStateManager(const TrackerStateManager&) = delete;
    TrackerStateManager& operator=(const TrackerStateManager&) = delete;

    struct InternalTrack {
        int          virtual_track_id = 0;
        cv::Point2f  centroid_curr;
        cv::Point2f  centroid_prev;
        cv::Rect2f   bbox_curr;
        int          last_zone_id = -1;
        int          curr_zone_id = -1;
        int          person_id    = -1;
        int          worker_track_id = -1;
        std::string  object_class;
        int64_t      first_seen_ms = 0;
        int64_t      last_seen_ms  = 0;
    };

    struct CameraState {
        std::unordered_map<int /*virtual_track_id*/, InternalTrack> tracks;
        int next_vid = 1;
        std::mutex mu;
    };

    CameraState& getOrCreate(int camera_id);
    static float iou(const cv::Rect2f& a, const cv::Rect2f& b);

    static constexpr float    kIouMatchThreshold  = 0.30f;
    static constexpr int64_t  kStaleAgeMs         = 5000;
    static constexpr std::size_t kMaxTracksPerCam = 200;  // safety cap

    std::shared_mutex by_cam_mu_;
    std::unordered_map<int, std::unique_ptr<CameraState>> by_camera_;
};

}  // namespace vms::core
