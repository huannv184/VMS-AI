// ==============================================================
// File: tests/test_ppe_status.cpp
// Inline reproduction of the health-classification matrix and
// PpeComplianceAggregator's behavioural-readiness atomics inside
// the GET /api/analytics/ppe/status handler.
//
// Mirrors the logic block in src/api/analytics_controller.cpp
// directly under the comment "Health derivation. The matrix below
// is mirrored byte-for-byte in tests/test_ppe_status.cpp". When
// the controller's matrix changes, update this file in lockstep.
//
// No DB / no Crow / no atomic context — pure logic test.
// ==============================================================

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// Mirror of the controller's inputs to the health decision.
struct Inputs {
    bool   env_force_enable   = false;
    bool   env_force_disable  = false;
    std::vector<int> enabled_cameras;        // from ai_config probe
    int64_t now_ms            = 1'700'000'000'000LL;
    int64_t last_tick_ms      = 0;
    std::size_t active_cameras= 0;
    int64_t stale_after_ms    = 5LL * 60 * 1000;  // 5 min default
};

// Mirror of the matrix in analytics_controller.cpp /ppe/status handler.
// Keep in lockstep with the production code — see the controller comment.
std::string deriveHealth(const Inputs& in) {
    const bool intent_enabled = in.env_force_enable
        || (!in.env_force_disable && !in.enabled_cameras.empty());
    if (!intent_enabled) return "no_cameras";
    if (in.last_tick_ms <= 0 || in.active_cameras == 0) return "inactive";
    if ((in.now_ms - in.last_tick_ms) > in.stale_after_ms) return "stale";
    return "ok";
}

// Mirror of PpeComplianceAggregator::recordTick's atomic-bump behaviour.
// The production class also writes ring slots — those are exercised in
// test_counter_bucket / test_line_crossing-style indirect tests. Here we
// only assert the readiness signals because /status reads them directly.
struct ReadinessAtomics {
    std::atomic<int64_t> last_tick_ms{0};
    std::atomic<int64_t> last_violation_ms{0};

    void recordTick(int64_t ts_ms, int compliant_count, int violating_count) {
        if (compliant_count <= 0 && violating_count <= 0) return;
        last_tick_ms.store(ts_ms, std::memory_order_relaxed);
        if (violating_count > 0) {
            last_violation_ms.store(ts_ms, std::memory_order_relaxed);
        }
    }
};

} // namespace

// ── Health classification matrix ────────────────────────────────────────────

TEST(PpeStatus, NoCamerasWhenNothingConfiguredAndNoEnvForce) {
    Inputs in;
    // Default: no cameras enabled in ai_config, no env force-enable.
    EXPECT_EQ(deriveHealth(in), "no_cameras");
}

TEST(PpeStatus, NoCamerasWhenEnvDisableTrumpsConfig) {
    Inputs in;
    in.enabled_cameras   = {1, 2, 3};
    in.env_force_disable = true;
    // Operator explicitly disabled PPE on the process — config doesn't matter,
    // and the badge must NOT lie that PPE is "active anywhere".
    EXPECT_EQ(deriveHealth(in), "no_cameras");
}

TEST(PpeStatus, EnvForceEnableBeatsEmptyConfig) {
    Inputs in;
    in.env_force_enable = true;
    // No camera config, but VMS_AI_ENABLE_PPE=1 says "PPE intended to run".
    // Without ticks, that's the inactive state — but it's NOT no_cameras.
    EXPECT_EQ(deriveHealth(in), "inactive");
}

TEST(PpeStatus, InactiveWhenConfiguredButNoTicks) {
    Inputs in;
    in.enabled_cameras = {7};
    // last_tick_ms == 0 means we have NEVER seen a tick. Could be: ai_worker
    // failed to load engine, or the PPE-enabled camera has had no
    // PPE-persons since process start. Backend can't tell which.
    EXPECT_EQ(deriveHealth(in), "inactive");
}

TEST(PpeStatus, InactiveWhenLastTickButZeroActiveCameras) {
    Inputs in;
    in.enabled_cameras = {7};
    in.last_tick_ms    = 1'700'000'000'000LL - 1000;  // 1s ago
    in.active_cameras  = 0;   // shouldn't ever happen with last_tick > 0, but defensive.
    EXPECT_EQ(deriveHealth(in), "inactive");
}

TEST(PpeStatus, StaleWhenLastTickOlderThanCutoff) {
    Inputs in;
    in.enabled_cameras = {7};
    in.active_cameras  = 1;
    in.last_tick_ms    = in.now_ms - (in.stale_after_ms + 1000);  // 1s past cutoff
    EXPECT_EQ(deriveHealth(in), "stale");
}

TEST(PpeStatus, OkWhenConfiguredAndRecentlyTicked) {
    Inputs in;
    in.enabled_cameras = {7, 8};
    in.active_cameras  = 2;
    in.last_tick_ms    = in.now_ms - 2000;  // 2s ago, well within 5min
    EXPECT_EQ(deriveHealth(in), "ok");
}

TEST(PpeStatus, BoundaryAtStaleCutoffStaysOk) {
    Inputs in;
    in.enabled_cameras = {7};
    in.active_cameras  = 1;
    in.last_tick_ms    = in.now_ms - in.stale_after_ms;  // exactly at cutoff
    // Production uses `>` not `>=` so equality stays in the ok bucket.
    EXPECT_EQ(deriveHealth(in), "ok");
}

TEST(PpeStatus, EnvForceEnableWithRecentTickIsOk) {
    Inputs in;
    in.env_force_enable = true;
    in.active_cameras   = 1;
    in.last_tick_ms     = in.now_ms - 1000;
    EXPECT_EQ(deriveHealth(in), "ok");
}

TEST(PpeStatus, CustomStaleCutoffRespected) {
    Inputs in;
    in.enabled_cameras = {1};
    in.active_cameras  = 1;
    in.stale_after_ms  = 30 * 1000;  // 30s
    in.last_tick_ms    = in.now_ms - 31 * 1000;  // 31s ago → over cutoff
    EXPECT_EQ(deriveHealth(in), "stale");
}

// ── Atomic-bump semantics ───────────────────────────────────────────────────

TEST(PpeReadinessAtomics, ZeroCountTickIsNoop) {
    ReadinessAtomics r;
    r.recordTick(/*ts*/1000, /*compliant*/0, /*violating*/0);
    EXPECT_EQ(r.last_tick_ms.load(),      0);
    EXPECT_EQ(r.last_violation_ms.load(), 0);
}

TEST(PpeReadinessAtomics, CompliantOnlyBumpsTickNotViolation) {
    ReadinessAtomics r;
    r.recordTick(/*ts*/2000, /*compliant*/5, /*violating*/0);
    EXPECT_EQ(r.last_tick_ms.load(),      2000);
    EXPECT_EQ(r.last_violation_ms.load(), 0);
}

TEST(PpeReadinessAtomics, ViolatingBumpsBoth) {
    ReadinessAtomics r;
    r.recordTick(/*ts*/3000, /*compliant*/2, /*violating*/1);
    EXPECT_EQ(r.last_tick_ms.load(),      3000);
    EXPECT_EQ(r.last_violation_ms.load(), 3000);
}

TEST(PpeReadinessAtomics, LastViolationStaysAtMostRecent) {
    ReadinessAtomics r;
    r.recordTick(3000, 0, 1);
    r.recordTick(4000, 5, 0);  // compliant tick after — must NOT reset violation
    r.recordTick(5000, 5, 0);
    EXPECT_EQ(r.last_tick_ms.load(),      5000);
    EXPECT_EQ(r.last_violation_ms.load(), 3000);  // unchanged
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
