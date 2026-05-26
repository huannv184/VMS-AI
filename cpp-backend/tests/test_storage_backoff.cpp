// Pins the H7 storage recovery backoff schedule. The long-lived retry
// loop in storage_manager.cpp::runBackoffRetryLoop calls
// StorageManager::nextBackoffSeconds(attempt) to compute the sleep
// window between retries. If anyone shortens this sequence carelessly,
// the loop hammers MinIO and inflates failure-cascade load; if anyone
// lengthens it without thinking, recovery time after an outage stretches
// past operator expectations. Both directions matter, so this test pins
// the exact sequence as a contract.
//
// Pure-function test: no curl, no real MinIO, no threads — just the
// integer sequence.

#include <gtest/gtest.h>

#include "utils/storage_manager.h"

using vms::utils::StorageManager;

TEST(StorageBackoff, FirstRetryIsFiveSeconds) {
    // attempt=1 is the FIRST retry after the initial try-once at boot.
    // Five seconds is short enough that an operator who just started
    // MinIO after vms_backend doesn't wait long for recovery, but long
    // enough that we're not hammering an obviously-broken endpoint.
    EXPECT_EQ(StorageManager::nextBackoffSeconds(1), 5);
}

TEST(StorageBackoff, SecondRetryIsFifteenSeconds) {
    EXPECT_EQ(StorageManager::nextBackoffSeconds(2), 15);
}

TEST(StorageBackoff, ThirdRetryIsOneMinute) {
    EXPECT_EQ(StorageManager::nextBackoffSeconds(3), 60);
}

TEST(StorageBackoff, FourthRetryIsFiveMinutes) {
    EXPECT_EQ(StorageManager::nextBackoffSeconds(4), 300);
}

TEST(StorageBackoff, CapsAtFiveMinutesIndefinitely) {
    // Once we hit the cap, we stay there forever. Two checks: just past
    // the cap, and far past the cap, must both return 300.
    EXPECT_EQ(StorageManager::nextBackoffSeconds(5), 300);
    EXPECT_EQ(StorageManager::nextBackoffSeconds(100), 300);
    EXPECT_EQ(StorageManager::nextBackoffSeconds(1000000), 300);
}

TEST(StorageBackoff, ZeroOrNegativeAttemptDoesNotCrash) {
    // The retry loop is 1-indexed, but defensive contract: attempt <= 1
    // should return the first-retry value, not zero or negative seconds.
    // A zero sleep would spin-loop and pin a CPU; a negative would
    // sleep_for() reject and the loop would tight-spin.
    EXPECT_EQ(StorageManager::nextBackoffSeconds(0), 5);
    EXPECT_EQ(StorageManager::nextBackoffSeconds(-1), 5);
    EXPECT_EQ(StorageManager::nextBackoffSeconds(-1000), 5);
}

TEST(StorageBackoff, MonotonicallyNonDecreasing) {
    // Property test: across the meaningful range, the sequence never
    // shrinks. Guards against someone "tuning" the schedule into a
    // non-monotonic shape that confuses operators reading logs.
    int prev = StorageManager::nextBackoffSeconds(1);
    for (int i = 2; i <= 20; ++i) {
        int cur = StorageManager::nextBackoffSeconds(i);
        EXPECT_GE(cur, prev) << "Backoff shrank between attempt " << (i - 1) << " and " << i;
        prev = cur;
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
