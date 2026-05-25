// ==============================================================
// File: tests/test_attendance_health.cpp
// Inline reproduction of the health-classification matrix in the
// GET /api/attendance/health handler (attendance_controller.cpp).
//
// Mirrors the logic block under the comment "Health derivation.
// The matrix below is mirrored byte-for-byte in
// tests/test_attendance_health.cpp" — update both in lockstep.
//
// No DB / no Crow — pure decision-table test.
// ==============================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

// Mirror of the controller's inputs to the health decision.
struct Inputs {
    bool     started          = true;
    int64_t  now_s            = 1'700'000'000LL;
    int64_t  last_event_ts    = 0;
    int64_t  stale_after_s    = 3600;             // 1h default
    uint64_t unlinked_24h     = 0;
    uint64_t fallback_24h     = 0;
};

// Mirror of the matrix in attendance_controller.cpp /attendance/health.
// Keep in lockstep with the production code — see the controller comment.
std::string deriveHealth(const Inputs& in) {
    if (!in.started) return "inactive";
    if (in.unlinked_24h > 0 || in.fallback_24h > 0) return "degraded";
    if (in.last_event_ts > 0 && (in.now_s - in.last_event_ts) > in.stale_after_s) {
        return "stale";
    }
    return "ok";
}

} // namespace

// ── inactive ───────────────────────────────────────────────────────────────

TEST(AttendanceHealth, InactiveWhenNotStarted) {
    Inputs in;
    in.started = false;
    EXPECT_EQ(deriveHealth(in), "inactive");
}

TEST(AttendanceHealth, InactiveTrumpsDegradedAndStale) {
    Inputs in;
    in.started        = false;
    in.unlinked_24h   = 5;
    in.fallback_24h   = 10;
    in.last_event_ts  = in.now_s - in.stale_after_s - 1;  // would be stale
    // !started short-circuits — config-gap and staleness become moot.
    EXPECT_EQ(deriveHealth(in), "inactive");
}

// ── degraded ───────────────────────────────────────────────────────────────

TEST(AttendanceHealth, DegradedWhenUnlinkedRecognitions) {
    Inputs in;
    in.unlinked_24h = 3;
    // Person recognized but no employees row — operator hasn't bound the
    // face DB person_id to an employee. Visible config gap.
    EXPECT_EQ(deriveHealth(in), "degraded");
}

TEST(AttendanceHealth, DegradedWhenFallbackRule) {
    Inputs in;
    in.fallback_24h = 7;
    // Camera has no role configured — source_rule fell back to
    // min_max_fallback. In/out attribution is heuristic, not authoritative.
    EXPECT_EQ(deriveHealth(in), "degraded");
}

TEST(AttendanceHealth, DegradedTrumpsStale) {
    Inputs in;
    in.unlinked_24h  = 1;
    in.last_event_ts = in.now_s - in.stale_after_s - 60;  // would be stale
    // Visible config gap is the more actionable signal than "no recent
    // activity" — surface it first.
    EXPECT_EQ(deriveHealth(in), "degraded");
}

// ── stale ──────────────────────────────────────────────────────────────────

TEST(AttendanceHealth, StaleWhenLastEventOlderThanCutoff) {
    Inputs in;
    in.last_event_ts = in.now_s - in.stale_after_s - 1;
    EXPECT_EQ(deriveHealth(in), "stale");
}

TEST(AttendanceHealth, BoundaryAtStaleCutoffStaysOk) {
    Inputs in;
    in.last_event_ts = in.now_s - in.stale_after_s;  // exactly at cutoff
    // Production uses `>`, not `>=` — equality stays in the ok bucket.
    EXPECT_EQ(deriveHealth(in), "ok");
}

TEST(AttendanceHealth, CustomStaleCutoffRespected) {
    Inputs in;
    in.stale_after_s = 300;                    // operator tightened to 5 min
    in.last_event_ts = in.now_s - 301;         // 1s over the custom cutoff
    EXPECT_EQ(deriveHealth(in), "stale");
}

// ── ok ─────────────────────────────────────────────────────────────────────

TEST(AttendanceHealth, OkWhenStartedAndNeverEvent) {
    Inputs in;
    // last_event_ts == 0 → tracker has never accepted a recognition since
    // boot. Could be "freshly started, no one has walked through yet" — NOT
    // a failure mode. Status stays ok until either a degraded signal lands
    // or the cutoff lapses on a real prior event.
    EXPECT_EQ(deriveHealth(in), "ok");
}

TEST(AttendanceHealth, OkWhenStartedAndRecentEvent) {
    Inputs in;
    in.last_event_ts = in.now_s - 60;          // 1 minute ago
    EXPECT_EQ(deriveHealth(in), "ok");
}

TEST(AttendanceHealth, OkIgnoresZeroLastEventEvenAfterCutoff) {
    // last_event_ts == 0 but now_s - 0 obviously exceeds any cutoff —
    // the guard `last_event_ts > 0` prevents a false stale classification
    // on a freshly-booted tracker.
    Inputs in;
    in.last_event_ts = 0;
    in.now_s         = 1'700'000'000LL;
    in.stale_after_s = 60;                     // even a tight cutoff
    EXPECT_EQ(deriveHealth(in), "ok");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
