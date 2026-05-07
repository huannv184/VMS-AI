// ==============================================================
// File: tests/test_line_crossing.cpp
// Inline reproduction of PeopleCountTracker::checkCrossings core
// (mirrors src/core/people_count_tracker.cpp). Exercises geometry
// helpers, side-flip + segment-intersect gate, cooldown drop,
// direction labelling, object-class filter, and disabled lines.
//
// Inline reproduction — does NOT link against backend code so the
// test binary stays cheap. If you change checkCrossings or the
// helpers, update this fixture too.
// ==============================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ── Stand-ins for cv::Point2f / cv::Rect2f to avoid pulling OpenCV ────────
struct Pt   { float x; float y; bool operator==(const Pt& o) const { return x == o.x && y == o.y; } };
struct Rect { float x; float y; float w; float h; };

struct CountingLine {
    int    id        = 0;
    int    camera_id = 0;
    std::string name;
    float  ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f; // normalized 0..1
    std::string dir_a_label = "in";
    std::string dir_b_label = "out";
    std::vector<std::string> object_classes;            // empty = match all
    bool   enabled = true;
};

struct TrackState {
    int    virtual_track_id = 0;
    Pt     centroid_curr{};
    Pt     centroid_prev{};
    Rect   bbox_curr{};
    int    person_id = -1;
    std::string object_class;
    bool   is_new = false;
};

struct LineCrossing {
    int         line_id = 0;
    std::string line_name;
    std::string direction_label;
    std::string direction_code;
    int         virtual_track_id = 0;
    int         person_id = -1;
    std::string object_class;
    Rect        bbox{};
    int64_t     ts_ms = 0;
};

// ── Geometry helpers (verbatim from people_count_tracker.cpp) ─────────────
float signedSide(float px, float py, float ax, float ay, float bx, float by) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

bool segmentsIntersect(float p1x, float p1y, float p2x, float p2y,
                       float ax, float ay, float bx, float by) {
    const float d1 = signedSide(p1x, p1y, ax, ay, bx, by);
    const float d2 = signedSide(p2x, p2y, ax, ay, bx, by);
    const float d3 = signedSide(ax,  ay,  p1x, p1y, p2x, p2y);
    const float d4 = signedSide(bx,  by,  p1x, p1y, p2x, p2y);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return true;
    }
    return false;
}

// ── Inline reproduction of checkCrossings (single-camera, no DB) ──────────
struct CrossKey { int cam; int line; int vid;
    bool operator==(const CrossKey& o) const { return cam == o.cam && line == o.line && vid == o.vid; }
};
struct CrossKeyHash {
    std::size_t operator()(const CrossKey& k) const noexcept {
        std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.cam));
        h = h * 1315423911u + static_cast<std::uint32_t>(k.line);
        h = h * 1315423911u + static_cast<std::uint32_t>(k.vid);
        return static_cast<std::size_t>(h);
    }
};

struct Engine {
    int cooldown_ms = 3000;
    std::unordered_map<CrossKey, int64_t, CrossKeyHash> last_fire_ms;

    std::vector<LineCrossing> checkCrossings(int camera_id,
                                             const std::vector<TrackState>& tracks,
                                             const std::vector<CountingLine>& lines,
                                             int frame_w, int frame_h,
                                             int64_t ts_ms) {
        std::vector<LineCrossing> out;
        if (tracks.empty() || frame_w <= 0 || frame_h <= 0) return out;
        const float fw = static_cast<float>(frame_w);
        const float fh = static_cast<float>(frame_h);

        for (const auto& t : tracks) {
            if (t.is_new) continue;
            if (t.centroid_curr == t.centroid_prev) continue;

            for (const auto& ln : lines) {
                if (!ln.enabled) continue;
                if (!ln.object_classes.empty()) {
                    bool match = false;
                    for (const auto& cls : ln.object_classes) {
                        if (cls == t.object_class) { match = true; break; }
                    }
                    if (!match) continue;
                }

                const float ax = ln.ax * fw;
                const float ay = ln.ay * fh;
                const float bx = ln.bx * fw;
                const float by = ln.by * fh;

                const float prev_side = signedSide(t.centroid_prev.x, t.centroid_prev.y, ax, ay, bx, by);
                const float curr_side = signedSide(t.centroid_curr.x, t.centroid_curr.y, ax, ay, bx, by);

                const bool flipped = (prev_side > 0 && curr_side < 0) ||
                                     (prev_side < 0 && curr_side > 0);
                if (!flipped) continue;
                if (!segmentsIntersect(t.centroid_prev.x, t.centroid_prev.y,
                                       t.centroid_curr.x, t.centroid_curr.y,
                                       ax, ay, bx, by)) continue;

                CrossKey k{camera_id, ln.id, t.virtual_track_id};
                auto it = last_fire_ms.find(k);
                if (it != last_fire_ms.end() && (ts_ms - it->second) < cooldown_ms) {
                    continue;
                }
                last_fire_ms[k] = ts_ms;

                LineCrossing c;
                c.line_id          = ln.id;
                c.line_name        = ln.name;
                c.virtual_track_id = t.virtual_track_id;
                c.person_id        = t.person_id;
                c.object_class     = t.object_class;
                c.bbox             = t.bbox_curr;
                c.ts_ms            = ts_ms;
                if (prev_side > 0 && curr_side < 0) {
                    c.direction_code  = "a_to_b";
                    c.direction_label = ln.dir_b_label;
                } else {
                    c.direction_code  = "b_to_a";
                    c.direction_label = ln.dir_a_label;
                }
                out.push_back(std::move(c));
            }
        }
        return out;
    }
};

// ── Test fixtures ─────────────────────────────────────────────────────────
constexpr int FW = 1000;
constexpr int FH = 1000;

// Vertical line at x=0.5 → from (0.5, 0.0) to (0.5, 1.0).
// In pixel space: A=(500,0), B=(500,1000). signedSide at (px,py):
//   (bx-ax)*(py-ay) - (by-ay)*(px-ax)
// = 0*(py-0) - 1000*(px-500)
// = -1000 * (px - 500)
// → px<500 ⇒ side > 0  (left of line)
// → px>500 ⇒ side < 0  (right of line)
CountingLine makeVerticalLine(int id = 1, int cam = 7) {
    CountingLine ln;
    ln.id = id; ln.camera_id = cam; ln.name = "v_mid";
    ln.ax = 0.5f; ln.ay = 0.0f; ln.bx = 0.5f; ln.by = 1.0f;
    ln.dir_a_label = "in";
    ln.dir_b_label = "out";
    return ln;
}

TrackState makeTrack(int vid, float prev_x, float prev_y, float curr_x, float curr_y,
                     const std::string& cls = "person") {
    TrackState t;
    t.virtual_track_id = vid;
    t.centroid_prev    = {prev_x, prev_y};
    t.centroid_curr    = {curr_x, curr_y};
    t.bbox_curr        = {curr_x - 25, curr_y - 25, 50, 50};
    t.object_class     = cls;
    t.is_new           = false;
    return t;
}

} // namespace

// ── signedSide ────────────────────────────────────────────────────────────
TEST(LineCrossingGeometry, SignedSideLeftPositive) {
    // Vertical AB from (500,0) to (500,1000); point at (200,500) is to the left.
    EXPECT_GT(signedSide(200, 500, 500, 0, 500, 1000), 0.0f);
}

TEST(LineCrossingGeometry, SignedSideRightNegative) {
    EXPECT_LT(signedSide(800, 500, 500, 0, 500, 1000), 0.0f);
}

TEST(LineCrossingGeometry, SignedSideOnLineZero) {
    EXPECT_EQ(signedSide(500, 500, 500, 0, 500, 1000), 0.0f);
}

// ── segmentsIntersect ─────────────────────────────────────────────────────
TEST(LineCrossingGeometry, IntersectingSegments) {
    // Path crosses AB through midpoint.
    EXPECT_TRUE(segmentsIntersect(200, 500, 800, 500,   // P1→P2 horizontal
                                  500, 0,   500, 1000)); // A→B vertical
}

TEST(LineCrossingGeometry, ParallelSegmentsDoNotIntersect) {
    EXPECT_FALSE(segmentsIntersect(200, 100, 800, 100,
                                   200, 500, 800, 500));
}

TEST(LineCrossingGeometry, PathCrossesInfiniteLineButNotABSegment) {
    // Path crosses x=500 but at y=2000, AB only spans y=0..1000.
    EXPECT_FALSE(segmentsIntersect(200, 2000, 800, 2000,
                                   500, 0,    500, 1000));
}

TEST(LineCrossingGeometry, SegmentsTouchingEndpointsNotCounted) {
    // Strict-inequality gate (>0 && <0) means coincident endpoints don't fire —
    // intentional, prevents double-count from a track that briefly lands on the line.
    EXPECT_FALSE(segmentsIntersect(500, 500, 800, 500,
                                   500, 0,   500, 1000));
}

// ── Side flip + intersection gate ─────────────────────────────────────────
TEST(LineCrossingDetect, SimpleAtoB) {
    Engine e;
    auto ln = makeVerticalLine();
    // Track moves from x=200 (left, side>0) to x=800 (right, side<0): A→B.
    auto t = makeTrack(/*vid*/ 1, 200, 500, 800, 500);
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].direction_code,  "a_to_b");
    EXPECT_EQ(out[0].direction_label, "out"); // dir_b_label
    EXPECT_EQ(out[0].virtual_track_id, 1);
}

TEST(LineCrossingDetect, SimpleBtoA) {
    Engine e;
    auto ln = makeVerticalLine();
    // x=800 → x=200 means side<0 → side>0: B→A.
    auto t = makeTrack(/*vid*/ 2, 800, 500, 200, 500);
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].direction_code,  "b_to_a");
    EXPECT_EQ(out[0].direction_label, "in");  // dir_a_label
}

TEST(LineCrossingDetect, NoCrossingSameSide) {
    Engine e;
    auto ln = makeVerticalLine();
    auto t = makeTrack(1, 100, 500, 200, 500); // both left of line
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    EXPECT_TRUE(out.empty());
}

TEST(LineCrossingDetect, PathCrossesExtendedLineButOutsideAB) {
    Engine e;
    auto ln = makeVerticalLine();
    // Path goes from (200, 1500) to (800, 1500) — crosses x=500 but at y=1500,
    // AB only spans y=0..1000. signedSide will flip but segmentsIntersect filter blocks.
    auto t = makeTrack(1, 200, 1500, 800, 1500);
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    EXPECT_TRUE(out.empty());
}

TEST(LineCrossingDetect, IsNewTrackSkipped) {
    Engine e;
    auto ln = makeVerticalLine();
    auto t = makeTrack(1, 200, 500, 800, 500);
    t.is_new = true; // first frame this track was seen — no real prev centroid
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    EXPECT_TRUE(out.empty());
}

TEST(LineCrossingDetect, StationaryTrackSkipped) {
    Engine e;
    auto ln = makeVerticalLine();
    auto t = makeTrack(1, 500, 500, 500, 500); // didn't move
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    EXPECT_TRUE(out.empty());
}

TEST(LineCrossingDetect, DisabledLineIgnored) {
    Engine e;
    auto ln = makeVerticalLine();
    ln.enabled = false;
    auto t = makeTrack(1, 200, 500, 800, 500);
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    EXPECT_TRUE(out.empty());
}

// ── Cooldown ──────────────────────────────────────────────────────────────
TEST(LineCrossingCooldown, SameTrackWithinWindowDropped) {
    Engine e;
    e.cooldown_ms = 3000;
    auto ln = makeVerticalLine();
    auto t1 = makeTrack(1, 200, 500, 800, 500);
    auto t2 = makeTrack(1, 800, 500, 200, 500); // wiggles back

    auto out1 = e.checkCrossings(7, {t1}, {ln}, FW, FH, 1'000'000);
    auto out2 = e.checkCrossings(7, {t2}, {ln}, FW, FH, 1'001'000); // 1s later
    ASSERT_EQ(out1.size(), 1u);
    EXPECT_TRUE(out2.empty());
}

TEST(LineCrossingCooldown, SameTrackAfterWindowAccepted) {
    Engine e;
    e.cooldown_ms = 3000;
    auto ln = makeVerticalLine();
    auto t1 = makeTrack(1, 200, 500, 800, 500);
    auto t2 = makeTrack(1, 800, 500, 200, 500);

    auto out1 = e.checkCrossings(7, {t1}, {ln}, FW, FH, 1'000'000);
    auto out2 = e.checkCrossings(7, {t2}, {ln}, FW, FH, 1'003'500); // 3.5s later
    ASSERT_EQ(out1.size(), 1u);
    ASSERT_EQ(out2.size(), 1u);
    EXPECT_EQ(out2[0].direction_code, "b_to_a");
}

TEST(LineCrossingCooldown, DifferentTracksNotShared) {
    Engine e;
    e.cooldown_ms = 3000;
    auto ln = makeVerticalLine();
    auto t1 = makeTrack(/*vid*/ 1, 200, 500, 800, 500);
    auto t2 = makeTrack(/*vid*/ 2, 200, 500, 800, 500);
    // Two separate tracks crossing simultaneously — both fire.
    auto out = e.checkCrossings(7, {t1, t2}, {ln}, FW, FH, 1'000'000);
    EXPECT_EQ(out.size(), 2u);
}

TEST(LineCrossingCooldown, DifferentLinesShareTrackId) {
    // Same vid, but two distinct lines → independent cooldown buckets.
    Engine e;
    e.cooldown_ms = 3000;
    auto ln1 = makeVerticalLine(/*id*/ 1);
    CountingLine ln2 = ln1;
    ln2.id = 2;
    ln2.ax = 0.7f; ln2.bx = 0.7f; // second vertical line at x=700
    auto t = makeTrack(1, 200, 500, 900, 500); // crosses both
    auto out = e.checkCrossings(7, {t}, {ln1, ln2}, FW, FH, 1'000'000);
    EXPECT_EQ(out.size(), 2u);
}

// ── Object class filter ───────────────────────────────────────────────────
TEST(LineCrossingFilter, EmptyClassesMatchesAll) {
    Engine e;
    auto ln = makeVerticalLine();
    ln.object_classes.clear();
    auto t = makeTrack(1, 200, 500, 800, 500, "car");
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    EXPECT_EQ(out.size(), 1u);
}

TEST(LineCrossingFilter, NonMatchingClassDropped) {
    Engine e;
    auto ln = makeVerticalLine();
    ln.object_classes = {"person"};
    auto t = makeTrack(1, 200, 500, 800, 500, "car");
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    EXPECT_TRUE(out.empty());
}

TEST(LineCrossingFilter, MatchingClassFiresEvent) {
    Engine e;
    auto ln = makeVerticalLine();
    ln.object_classes = {"person", "car"};
    auto t = makeTrack(1, 200, 500, 800, 500, "car");
    auto out = e.checkCrossings(7, {t}, {ln}, FW, FH, 1'000'000);
    EXPECT_EQ(out.size(), 1u);
}

// ── Multi-camera isolation ────────────────────────────────────────────────
TEST(LineCrossingDetect, CrossingsScopedByCamera) {
    // Same vid value on two cameras → cooldown does not bleed across.
    Engine e;
    e.cooldown_ms = 3000;
    auto ln = makeVerticalLine();
    auto t1 = makeTrack(1, 200, 500, 800, 500);
    auto out_cam7 = e.checkCrossings(7,  {t1}, {ln}, FW, FH, 1'000'000);
    auto out_cam9 = e.checkCrossings(9,  {t1}, {ln}, FW, FH, 1'000'500);
    EXPECT_EQ(out_cam7.size(), 1u);
    EXPECT_EQ(out_cam9.size(), 1u);
}
