// ==============================================================
// File: src/core/tracker_state_manager.cpp
// ==============================================================

#include "core/tracker_state_manager.h"
#include "utils/logger.h"

#include <algorithm>
#include <utility>

namespace vms::core {

// ── DetectionInput::fromJson ──────────────────────────────────────────────
DetectionInput DetectionInput::fromJson(const nlohmann::json& obj) {
    DetectionInput d;
    d.class_id        = obj.value("class_id", -1);
    d.label           = obj.value("type", obj.value("class_name", std::string{}));
    d.confidence      = obj.value("confidence", 0.0f);
    d.worker_track_id = obj.value("track_id", -1);
    d.person_id       = obj.value("person_id", -1);

    if (obj.contains("bbox") && obj["bbox"].is_array() && obj["bbox"].size() >= 4) {
        const float x1 = obj["bbox"][0].get<float>();
        const float y1 = obj["bbox"][1].get<float>();
        const float x2 = obj["bbox"][2].get<float>();
        const float y2 = obj["bbox"][3].get<float>();
        d.bbox = cv::Rect2f(x1, y1, std::max(0.0f, x2 - x1), std::max(0.0f, y2 - y1));
    }
    return d;
}

// ── Singleton ─────────────────────────────────────────────────────────────
TrackerStateManager& TrackerStateManager::getInstance() {
    static TrackerStateManager inst;
    return inst;
}

TrackerStateManager::CameraState& TrackerStateManager::getOrCreate(int camera_id) {
    {
        std::shared_lock<std::shared_mutex> rd(by_cam_mu_);
        auto it = by_camera_.find(camera_id);
        if (it != by_camera_.end()) return *it->second;
    }
    std::unique_lock<std::shared_mutex> wr(by_cam_mu_);
    auto& slot = by_camera_[camera_id];
    if (!slot) slot = std::make_unique<CameraState>();
    return *slot;
}

float TrackerStateManager::iou(const cv::Rect2f& a, const cv::Rect2f& b) {
    const float inter_x = std::max(a.x, b.x);
    const float inter_y = std::max(a.y, b.y);
    const float inter_r = std::min(a.x + a.width,  b.x + b.width);
    const float inter_b = std::min(a.y + a.height, b.y + b.height);
    const float iw = inter_r - inter_x;
    const float ih = inter_b - inter_y;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    const float inter = iw * ih;
    const float uni   = a.area() + b.area() - inter;
    return uni > 0.0f ? (inter / uni) : 0.0f;
}

// ── updateFrame: greedy IoU matching + GC ────────────────────────────────
std::vector<TrackState> TrackerStateManager::updateFrame(
        int camera_id,
        const std::vector<DetectionInput>& dets,
        int64_t ts_ms) {

    std::vector<TrackState> out;
    if (dets.empty()) {
        // Still GC stale tracks even on empty frames.
        auto& cam = getOrCreate(camera_id);
        std::lock_guard<std::mutex> lk(cam.mu);
        for (auto it = cam.tracks.begin(); it != cam.tracks.end(); ) {
            if (ts_ms - it->second.last_seen_ms > kStaleAgeMs) it = cam.tracks.erase(it);
            else ++it;
        }
        return out;
    }

    auto& cam = getOrCreate(camera_id);
    std::lock_guard<std::mutex> lk(cam.mu);

    // Build a list of currently-alive tracks (not stale yet).
    std::vector<int> alive_ids;
    alive_ids.reserve(cam.tracks.size());
    for (auto& [vid, t] : cam.tracks) {
        if (ts_ms - t.last_seen_ms <= kStaleAgeMs) alive_ids.push_back(vid);
    }

    const std::size_t N = dets.size();
    const std::size_t M = alive_ids.size();

    std::vector<bool> det_used(N, false);
    std::vector<bool> trk_used(M, false);

    // Greedy: pick highest IoU pair until exhausted or below threshold.
    // For N, M ≤ ~30 this is trivially fast.
    while (true) {
        float best_iou = kIouMatchThreshold;
        int best_d = -1, best_t = -1;
        for (std::size_t i = 0; i < N; ++i) {
            if (det_used[i]) continue;
            for (std::size_t j = 0; j < M; ++j) {
                if (trk_used[j]) continue;
                const auto& trk = cam.tracks[alive_ids[j]];
                const float v = iou(dets[i].bbox, trk.bbox_curr);
                if (v > best_iou) {
                    best_iou = v;
                    best_d = static_cast<int>(i);
                    best_t = static_cast<int>(j);
                }
            }
        }
        if (best_d < 0) break;
        det_used[best_d] = true;
        trk_used[best_t] = true;

        InternalTrack& trk = cam.tracks[alive_ids[best_t]];
        trk.centroid_prev   = trk.centroid_curr;
        trk.bbox_curr       = dets[best_d].bbox;
        trk.centroid_curr   = cv::Point2f(trk.bbox_curr.x + trk.bbox_curr.width  * 0.5f,
                                          trk.bbox_curr.y + trk.bbox_curr.height * 0.5f);
        trk.last_seen_ms    = ts_ms;
        if (dets[best_d].person_id > 0) trk.person_id = dets[best_d].person_id;
        if (dets[best_d].worker_track_id > 0) trk.worker_track_id = dets[best_d].worker_track_id;
        if (!dets[best_d].label.empty()) trk.object_class = dets[best_d].label;

        TrackState s;
        s.virtual_track_id = trk.virtual_track_id;
        s.centroid_curr    = trk.centroid_curr;
        s.centroid_prev    = trk.centroid_prev;
        s.bbox_curr        = trk.bbox_curr;
        s.last_zone_id     = trk.last_zone_id;
        s.curr_zone_id     = trk.curr_zone_id;  // updated by PeopleCountTracker via setZone()
        s.person_id        = trk.person_id;
        s.worker_track_id  = trk.worker_track_id;
        s.object_class     = trk.object_class;
        s.first_seen_ms    = trk.first_seen_ms;
        s.last_seen_ms     = trk.last_seen_ms;
        s.is_new           = false;
        out.push_back(std::move(s));
    }

    // Unmatched detections → spawn new tracks (subject to safety cap).
    for (std::size_t i = 0; i < N; ++i) {
        if (det_used[i]) continue;
        if (cam.tracks.size() >= kMaxTracksPerCam) {
            LOG_THROTTLED_WARN(10000,
                "TrackerStateManager: cam={} reached max_tracks={}, dropping new det",
                camera_id, kMaxTracksPerCam);
            break;
        }
        InternalTrack t;
        t.virtual_track_id = cam.next_vid++;
        t.bbox_curr        = dets[i].bbox;
        t.centroid_curr    = cv::Point2f(t.bbox_curr.x + t.bbox_curr.width  * 0.5f,
                                         t.bbox_curr.y + t.bbox_curr.height * 0.5f);
        t.centroid_prev    = t.centroid_curr;  // no history yet
        t.first_seen_ms    = ts_ms;
        t.last_seen_ms     = ts_ms;
        t.person_id        = dets[i].person_id > 0 ? dets[i].person_id : -1;
        t.worker_track_id  = dets[i].worker_track_id;
        t.object_class     = dets[i].label;
        cam.tracks[t.virtual_track_id] = t;

        TrackState s;
        s.virtual_track_id = t.virtual_track_id;
        s.centroid_curr    = t.centroid_curr;
        s.centroid_prev    = t.centroid_prev;
        s.bbox_curr        = t.bbox_curr;
        s.person_id        = t.person_id;
        s.worker_track_id  = t.worker_track_id;
        s.object_class     = t.object_class;
        s.first_seen_ms    = t.first_seen_ms;
        s.last_seen_ms     = t.last_seen_ms;
        s.is_new           = true;
        out.push_back(std::move(s));
    }

    // GC: drop tracks not seen in kStaleAgeMs (unmatched and stale).
    for (auto it = cam.tracks.begin(); it != cam.tracks.end(); ) {
        if (ts_ms - it->second.last_seen_ms > kStaleAgeMs) it = cam.tracks.erase(it);
        else ++it;
    }

    return out;
}

void TrackerStateManager::resetCamera(int camera_id) {
    std::shared_lock<std::shared_mutex> rd(by_cam_mu_);
    auto it = by_camera_.find(camera_id);
    if (it == by_camera_.end()) return;
    std::lock_guard<std::mutex> lk(it->second->mu);
    it->second->tracks.clear();
}

std::size_t TrackerStateManager::trackCount(int camera_id) {
    std::shared_lock<std::shared_mutex> rd(by_cam_mu_);
    auto it = by_camera_.find(camera_id);
    if (it == by_camera_.end()) return 0;
    std::lock_guard<std::mutex> lk(it->second->mu);
    return it->second->tracks.size();
}

}  // namespace vms::core
