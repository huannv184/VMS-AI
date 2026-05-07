// ==============================================================
// File: include/core/people_count_tracker.h
// Per-camera virtual line crossing detector. Consumes the stable
// TrackState output from TrackerStateManager and emits a crossing
// for every track whose centroid trajectory (prev→curr) intersects
// an enabled `counting_lines` row.
//
// Cache:
//   counting_lines is loaded once on first use and cached in memory.
//   Call reloadFromDb() after any /api/counter/lines mutation.
//
// Anti-spam:
//   Per (camera, line, virtual_track_id) cooldown — default 3s, env
//   VMS_COUNTER_CROSSING_COOLDOWN_MS. Prevents a track wiggling near
//   the line from generating duplicate events on consecutive frames.
//
// Thread-safety:
//   - cache: shared_mutex (read on hot path, write on reload)
//   - cooldown map: mutex (small map, short critical section)
// ==============================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/tracker_state_manager.h"

namespace vms::core {

struct CountingLine {
    int          id            = 0;
    int          camera_id     = 0;
    std::string  name;
    float        ax            = 0.0f;   // normalized 0..1
    float        ay            = 0.0f;
    float        bx            = 0.0f;
    float        by            = 0.0f;
    std::string  dir_a_label;            // e.g. "in"
    std::string  dir_b_label;            // e.g. "out"
    std::vector<std::string> object_classes;  // empty = match all
    bool         enabled       = true;
};

struct LineCrossing {
    int          line_id          = 0;
    std::string  line_name;
    std::string  direction_label; // user label ("in" / "out" / ...)
    std::string  direction_code;  // "a_to_b" or "b_to_a"
    int          virtual_track_id = 0;
    int          person_id        = -1;
    std::string  object_class;
    cv::Rect2f   bbox;
    int64_t      ts_ms            = 0;
};

class PeopleCountTracker {
public:
    static PeopleCountTracker& getInstance();

    // Reload counting_lines cache from DB. Safe to call from any thread.
    void reloadFromDb();

    // Inspect tracker output for line crossings. Returns empty vector when
    // the camera has no enabled counting lines.
    std::vector<LineCrossing> checkCrossings(
            int camera_id,
            const std::vector<TrackState>& tracks,
            int frame_w,
            int frame_h,
            int64_t ts_ms);

    // For tests / debugging.
    std::size_t lineCount(int camera_id);

private:
    PeopleCountTracker();
    ~PeopleCountTracker() = default;
    PeopleCountTracker(const PeopleCountTracker&) = delete;
    PeopleCountTracker& operator=(const PeopleCountTracker&) = delete;

    void ensureLoaded();

    // Cross product sign: >0 if P left of AB, <0 if right, 0 if on line.
    static float signedSide(float px, float py, float ax, float ay, float bx, float by);

    // Segment intersection — both prev/curr line and the counting line A-B.
    static bool segmentsIntersect(float p1x, float p1y, float p2x, float p2y,
                                  float ax, float ay, float bx, float by);

    int  cooldown_ms_;
    std::atomic<bool> loaded_{false};

    // camera_id → list of enabled lines
    mutable std::shared_mutex                                      cache_mu_;
    std::unordered_map<int, std::vector<CountingLine>>             by_camera_;

    // (camera_id, line_id, virtual_track_id) → last fire ts_ms
    struct CrossKey { int cam; int line; int vid; };
    struct CrossKeyHash {
        std::size_t operator()(const CrossKey& k) const noexcept {
            // mix three ints — collisions are fine here, only used for cooldown
            std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.cam));
            h = h * 1315423911u + static_cast<std::uint32_t>(k.line);
            h = h * 1315423911u + static_cast<std::uint32_t>(k.vid);
            return static_cast<std::size_t>(h);
        }
    };
    struct CrossKeyEq {
        bool operator()(const CrossKey& a, const CrossKey& b) const noexcept {
            return a.cam == b.cam && a.line == b.line && a.vid == b.vid;
        }
    };

    std::mutex cooldown_mu_;
    std::unordered_map<CrossKey, int64_t, CrossKeyHash, CrossKeyEq> last_fire_ms_;
};

} // namespace vms::core
