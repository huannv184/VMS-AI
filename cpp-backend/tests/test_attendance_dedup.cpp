// Was an inline reproduction of the AttendanceTracker dedup core. Now exercises
// the REAL helpers exposed in core/attendance_tracker.h:
//   - AttendanceTracker::packKey (already public, static inline)
//   - vms::core::isOutsideCooldownWindow (new pure helper)
//   - vms::core::kindForRole (new pure helper)
//
// onFaceRecognized itself can't be unit-tested without DbManager + a started
// BulkWriter thread — but it now calls the same helpers exercised here, so a
// drift between dedup contract and prod behaviour is impossible.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "core/attendance_tracker.h"

using vms::core::AttendanceTracker;
using vms::core::isOutsideCooldownWindow;
using vms::core::kindForRole;

// ── packKey (real impl, static inline in header) ──────────────────────────
TEST(AttendanceDedup, PackKeyDistinguishesPerson) {
    EXPECT_NE(AttendanceTracker::packKey(1, 5),
              AttendanceTracker::packKey(2, 5));
}

TEST(AttendanceDedup, PackKeyDistinguishesCamera) {
    EXPECT_NE(AttendanceTracker::packKey(7, 1),
              AttendanceTracker::packKey(7, 2));
}

TEST(AttendanceDedup, PackKeyHandlesNegativeIdsWithoutCollision) {
    // Negative ids should never reach the dedup map (person_id <= 0 is
    // rejected upstream), but bit-pack must still keep different sign-flips
    // distinct so a future caller can't accidentally collapse them.
    EXPECT_NE(AttendanceTracker::packKey(-1, 5),
              AttendanceTracker::packKey(5, -1));
}

// ── isOutsideCooldownWindow (real helper) ─────────────────────────────────
TEST(AttendanceDedup, FirstPunchPasses) {
    // No previous timestamp → caller short-circuits before calling helper;
    // this asserts the helper itself for prev_ts==0 case.
    EXPECT_TRUE(isOutsideCooldownWindow(0, 1000, 60));
}

TEST(AttendanceDedup, WithinCooldownDropped) {
    // 30s after the last punch with a 60s cooldown → must drop.
    EXPECT_FALSE(isOutsideCooldownWindow(1000, 1030, 60));
}

TEST(AttendanceDedup, ExactlyAtCooldownAccepted) {
    // Boundary: ts == prev + cooldown should pass (>= comparison).
    EXPECT_TRUE(isOutsideCooldownWindow(1000, 1060, 60));
}

TEST(AttendanceDedup, AfterCooldownAccepted) {
    EXPECT_TRUE(isOutsideCooldownWindow(1000, 1061, 60));
}

TEST(AttendanceDedup, ZeroCooldownAlwaysAccepts) {
    // Operationally weird (no dedup at all) but the helper must not crash
    // on a 0 cooldown — useful for tests / debug overrides.
    EXPECT_TRUE(isOutsideCooldownWindow(1000, 1000, 0));
}

// ── kindForRole (real helper) ─────────────────────────────────────────────
TEST(AttendanceDedup, EntryRoleAlwaysIn) {
    EXPECT_EQ(kindForRole("entry", ""),     "in");
    EXPECT_EQ(kindForRole("entry", "in"),   "in");
    EXPECT_EQ(kindForRole("entry", "out"),  "in");
}

TEST(AttendanceDedup, ExitRoleAlwaysOut) {
    EXPECT_EQ(kindForRole("exit", ""),     "out");
    EXPECT_EQ(kindForRole("exit", "in"),   "out");
    EXPECT_EQ(kindForRole("exit", "out"),  "out");
}

TEST(AttendanceDedup, BothRoleTogglesInOut) {
    // First punch (no prior) → "in"; subsequent toggle in/out/in.
    EXPECT_EQ(kindForRole("both", ""),    "in");
    EXPECT_EQ(kindForRole("both", "in"),  "out");
    EXPECT_EQ(kindForRole("both", "out"), "in");
}

TEST(AttendanceDedup, ObserveRoleEmitsSeen) {
    EXPECT_EQ(kindForRole("observe", ""),    "seen");
    EXPECT_EQ(kindForRole("observe", "in"),  "seen");
}

TEST(AttendanceDedup, UnconfiguredRoleEmitsSeen) {
    EXPECT_EQ(kindForRole("",       ""), "seen");
    EXPECT_EQ(kindForRole("garbage", ""), "seen");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
