// 2026-06-01 BUG-DB-WAL-UNBOUNDED-GROWTH-01 regression target.
// Pins the policy matrix for shouldRunWalCheckpoint(driver, seconds):
//   driver       seconds   expected
//   ----------   -------   --------
//   "sqlite"     60        true   (default; truncates ~once/min)
//   "sqlite"     1         true   (operator override for aggressive maintenance)
//   "sqlite"     0         false  (sentinel: opt out)
//   "sqlite"     -1        false  (negative treated as disabled, not a tick)
//   "postgresql" 60        false  (no WAL pragma equivalent — Postgres has its own checkpointer)
//   ""           60        false  (defensive: empty/garbage driver string)
//   "SQLite"     60        false  (case-sensitive on purpose — config layer normalises to lower-case)
//
// Why a test for two lines? Same reason as test_metrics_auth / test_hsts_header:
// the FIRST time someone "cleans up" the comparison (e.g. case-insensitive,
// or removes the > 0 check thinking 0 means "default to 60"), the production
// thread either spawns where it shouldn't (Postgres) or no-ops silently with
// zero on-call signal.

#include <gtest/gtest.h>

#include "database/db_manager.h"

using vms::database::shouldRunWalCheckpoint;

TEST(WalCheckpointPolicy, SqliteWithDefaultIntervalEnabled) {
    EXPECT_TRUE(shouldRunWalCheckpoint("sqlite", 60));
}

TEST(WalCheckpointPolicy, SqliteWithAggressiveIntervalEnabled) {
    EXPECT_TRUE(shouldRunWalCheckpoint("sqlite", 1));
}

TEST(WalCheckpointPolicy, SqliteZeroDisables) {
    EXPECT_FALSE(shouldRunWalCheckpoint("sqlite", 0));
}

TEST(WalCheckpointPolicy, SqliteNegativeDisables) {
    // A misconfigured operator yaml (negative int) must be treated as "off",
    // NOT silently coerced to the default — fail-loud surfaces the typo.
    EXPECT_FALSE(shouldRunWalCheckpoint("sqlite", -1));
}

TEST(WalCheckpointPolicy, PostgresAlwaysDisabled) {
    // Postgres has its own checkpointer (background_writer). The PRAGMA
    // doesn't exist on QPSQL — spawning the thread would just fail every
    // tick. Skip cleanly.
    EXPECT_FALSE(shouldRunWalCheckpoint("postgresql", 60));
}

TEST(WalCheckpointPolicy, EmptyDriverDisabled) {
    EXPECT_FALSE(shouldRunWalCheckpoint("", 60));
}

TEST(WalCheckpointPolicy, MixedCaseDriverDisabled) {
    // Config layer is responsible for canonicalising; this layer requires
    // exact match. If someone changes that contract, a config-side
    // normalisation regression silently disables the thread — this test
    // catches it.
    EXPECT_FALSE(shouldRunWalCheckpoint("SQLite", 60));
    EXPECT_FALSE(shouldRunWalCheckpoint("SQLITE", 60));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
