// Pure-function tests for the liveness/readiness split landed alongside
// the legacy /api/health soft-deprecation. The Crow route handler in
// system_controller.cpp calls `computeReady(snapshot)` with values read
// from process globals (atomics + StorageManager singleton). This test
// pins the policy matrix — DB always required, storage required only
// when the snapshot says so — so future refactors can't silently flip
// the boolean logic without a red ctest line.

#include <gtest/gtest.h>

#include "core/readiness_state.h"

using vms::core::ReadinessSnapshot;
using vms::core::computeReady;

TEST(ReadinessState, DefaultSnapshotIsNotReady) {
    // Default-constructed snapshot has every flag false. A backend that
    // has not yet loaded the DB must NOT report itself ready.
    ReadinessSnapshot s{};
    EXPECT_FALSE(computeReady(s));
}

TEST(ReadinessState, DbReadyStorageReadyStorageOptional) {
    ReadinessSnapshot s{};
    s.db_ready = true;
    s.storage_ready = true;
    s.storage_required = false;
    EXPECT_TRUE(computeReady(s));
}

TEST(ReadinessState, DbReadyStorageDownStorageOptional) {
    // Current production policy: storage outage is degraded-but-serving.
    // computeReady must stay true so the LB keeps the instance in rotation.
    ReadinessSnapshot s{};
    s.db_ready = true;
    s.storage_ready = false;
    s.storage_required = false;
    EXPECT_TRUE(computeReady(s));
}

TEST(ReadinessState, DbDownAlwaysNotReady) {
    // DB is non-negotiable. Regardless of storage flags, no DB means
    // 503 — the backend can't answer authenticated traffic without it.
    ReadinessSnapshot s{};
    s.db_ready = false;
    s.storage_ready = true;
    s.storage_required = false;
    EXPECT_FALSE(computeReady(s));

    s.storage_required = true;
    EXPECT_FALSE(computeReady(s));

    s.storage_ready = false;
    EXPECT_FALSE(computeReady(s));
}

TEST(ReadinessState, DbReadyStorageRequiredStorageDown) {
    // When the operator opts into "storage is required" (future P0 follow-up
    // for storage resilience), an outage flips readiness to 503. This is
    // the gate that future config-driven storage policy will toggle.
    ReadinessSnapshot s{};
    s.db_ready = true;
    s.storage_ready = false;
    s.storage_required = true;
    EXPECT_FALSE(computeReady(s));
}

TEST(ReadinessState, DbReadyStorageRequiredStorageUp) {
    ReadinessSnapshot s{};
    s.db_ready = true;
    s.storage_ready = true;
    s.storage_required = true;
    EXPECT_TRUE(computeReady(s));
}

TEST(ReadinessState, ComputeReadyIsPureAndDeterministic) {
    // Same snapshot evaluated twice must yield the same answer — guard
    // against anyone sneaking in a side-effect (clock, RNG, atomic write).
    ReadinessSnapshot s{};
    s.db_ready = true;
    s.storage_ready = true;
    const bool a = computeReady(s);
    const bool b = computeReady(s);
    EXPECT_EQ(a, b);

    s.db_ready = false;
    const bool c = computeReady(s);
    const bool d = computeReady(s);
    EXPECT_EQ(c, d);
    EXPECT_NE(a, c);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
