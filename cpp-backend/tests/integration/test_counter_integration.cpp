// ==============================================================
// File: tests/integration/test_counter_integration.cpp
// DB-backed integration test for CounterBucketAggregator (PR-7
// pilot, scope B-narrow per the readiness verification roadmap).
//
// What this test DOES cover (regression-locks for backend code
// changes that the inline test_counter_bucket.cpp can't catch):
//   - real-SQLite UPSERT semantics on counter_buckets_1m
//   - schema column/type compat between db_manager init and
//     the aggregator SQL + the /summary handler SQL
//   - per-bucket isolation across (camera, line, minute) keys
//   - time-window boundary behaviour of the sweep
//   - idempotency of re-running the sweep on stable input
//   - graceful skip on malformed metadata_json
//
// What this test does NOT cover (intentionally, per PR-7 scope):
//   - AiEventProcessor::processLineCrossings handler — bypassed by
//     inserting LINE_CROSSING events directly. The handler path is
//     covered by the hardware runbook (`docs/runbooks/counter_e2e_
//     readiness.md`) and indirectly by the test_line_crossing.cpp
//     geometry test.
//   - HTTP layer / Crow routing — covered by test_http_startup_
//     failfast.cpp at a different scope.
//
// Counterpart for PPE = test_ppe_integration.cpp (PR-7B, queued).
// ==============================================================

#include "integration_test_db.h"

#include "core/counter_bucket_aggregator.h"

#include <gtest/gtest.h>

#include <QCoreApplication>

#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace {

using vms::test::IntegrationTestDb;

// Helper — build the metadata_json payload the aggregator parses for
// LINE_CROSSING_* events. Mirrors what
// AiEventProcessor::processLineCrossings serialises into the events row.
std::string lineCrossingMeta(int line_id) {
    nlohmann::json m = {
        {"line_id", line_id},
        {"line_name", "test"},
        {"direction_code", "b_to_a"},
        {"virtual_track_id", 1}
    };
    return m.dump();
}

class CounterIntegration : public IntegrationTestDb {};

// ── Baseline ───────────────────────────────────────────────────────────────

TEST_F(CounterIntegration, SweepWithNoEventsCreatesNoBuckets) {
    vms::core::CounterBucketAggregator::getInstance().runSweepNow();
    EXPECT_EQ(countBuckets(), 0);
}

// ── Direction → in/out mapping ─────────────────────────────────────────────

TEST_F(CounterIntegration, BToACrossingIncrementsInCount) {
    const int64_t ts = wellInsideWindow();
    insertEvent(/*camera*/1, "LINE_CROSSING_B_TO_A", ts, lineCrossingMeta(10));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    ASSERT_EQ(countBuckets(/*camera*/1, /*source*/10), 1);
    const auto [in_c, out_c] = summarySumForCamera(
        /*camera*/1, /*from*/ts - 60, /*to*/ts + 600);
    EXPECT_EQ(in_c, 1);
    EXPECT_EQ(out_c, 0);
}

TEST_F(CounterIntegration, AToBCrossingIncrementsOutCount) {
    const int64_t ts = wellInsideWindow();
    insertEvent(/*camera*/1, "LINE_CROSSING_A_TO_B", ts, lineCrossingMeta(10));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    const auto [in_c, out_c] = summarySumForCamera(1, ts - 60, ts + 600);
    EXPECT_EQ(in_c, 0);
    EXPECT_EQ(out_c, 1);
}

// ── Aggregation in the same minute ─────────────────────────────────────────

TEST_F(CounterIntegration, MultipleCrossingsSameMinuteSumIntoOneBucket) {
    // Pick a base ts and stagger within ±20s — all three land in the same
    // minute-of-epoch bucket, so the aggregator should UPSERT once.
    const int64_t base = wellInsideWindow();
    const int64_t minute_start = (base / 60) * 60;
    insertEvent(1, "LINE_CROSSING_B_TO_A", minute_start +  5, lineCrossingMeta(10));
    insertEvent(1, "LINE_CROSSING_B_TO_A", minute_start + 25, lineCrossingMeta(10));
    insertEvent(1, "LINE_CROSSING_A_TO_B", minute_start + 45, lineCrossingMeta(10));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    ASSERT_EQ(countBuckets(1, 10), 1);
    const auto [in_c, out_c] = summarySumForCamera(1, base - 60, base + 600);
    EXPECT_EQ(in_c,  2);
    EXPECT_EQ(out_c, 1);
}

// ── Per-key isolation ──────────────────────────────────────────────────────

TEST_F(CounterIntegration, DifferentCamerasGetSeparateBuckets) {
    const int64_t ts = wellInsideWindow();
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, lineCrossingMeta(10));
    insertEvent(2, "LINE_CROSSING_B_TO_A", ts, lineCrossingMeta(10));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    EXPECT_EQ(countBuckets(/*camera*/1, /*source*/10), 1);
    EXPECT_EQ(countBuckets(/*camera*/2, /*source*/10), 1);
    EXPECT_EQ(summarySumForCamera(1, ts - 60, ts + 600).first, 1);
    EXPECT_EQ(summarySumForCamera(2, ts - 60, ts + 600).first, 1);
}

TEST_F(CounterIntegration, DifferentLinesOnSameCameraGetSeparateBuckets) {
    const int64_t ts = wellInsideWindow();
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, lineCrossingMeta(10));
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, lineCrossingMeta(11));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    EXPECT_EQ(countBuckets(/*camera*/1, /*source*/10), 1);
    EXPECT_EQ(countBuckets(/*camera*/1, /*source*/11), 1);
    // /summary sums across all lines for the camera → in_count = 2.
    EXPECT_EQ(summarySumForCamera(1, ts - 60, ts + 600).first, 2);
}

// ── Idempotency ────────────────────────────────────────────────────────────

TEST_F(CounterIntegration, ResweepDoesNotDoubleCount) {
    const int64_t ts = wellInsideWindow();
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, lineCrossingMeta(10));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();
    vms::core::CounterBucketAggregator::getInstance().runSweepNow();
    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    // ON CONFLICT … DO UPDATE SET re-writes the SAME count, so we should
    // see exactly one bucket with in_count=1 — not 3. This is the
    // regression that would surface if someone refactored the aggregator
    // to use INCREMENT-style writes instead of absolute-SET upserts.
    ASSERT_EQ(countBuckets(1, 10), 1);
    EXPECT_EQ(summarySumForCamera(1, ts - 60, ts + 600).first, 1);
}

// ── Robustness ─────────────────────────────────────────────────────────────

TEST_F(CounterIntegration, MalformedMetadataIsSilentlySkipped) {
    const int64_t ts = wellInsideWindow();
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, "{not json");                  // garbage
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, "{}");                          // no line_id
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, R"({"line_id":"abc"})");        // wrong type
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, R"({"line_id":-1})");           // invalid
    insertEvent(1, "LINE_CROSSING_B_TO_A", ts, lineCrossingMeta(10));          // valid

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    // Only the well-formed event should produce a bucket — the four bad
    // rows are dropped per `metadata_json` guard at counter_bucket_
    // aggregator.cpp:181-188 (try/catch + line_id<=0 filter).
    EXPECT_EQ(countBuckets(/*camera*/1, /*source*/10), 1);
    EXPECT_EQ(summarySumForCamera(1, ts - 60, ts + 600).first, 1);
}

// ── Time-window boundary ───────────────────────────────────────────────────

TEST_F(CounterIntegration, EventInsideLatenessMarginNotYetAggregated) {
    // Aggregator window = [now_min - 5min, now_min - 1min). Anything in
    // the most-recent minute (the "still being filled" zone) is skipped
    // until the next sweep, when its minute has fully elapsed.
    const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    insertEvent(1, "LINE_CROSSING_B_TO_A", now_s - 5, lineCrossingMeta(10));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    // Within the lateness margin → not in this sweep's window.
    EXPECT_EQ(countBuckets(/*camera*/1, /*source*/10), 0);
}

TEST_F(CounterIntegration, EventOlderThanLookbackNotAggregated) {
    // Default lookback = 5 min. An event 10 minutes old is outside the
    // re-aggregation window — sweep ignores it. This protects against a
    // late-arriving event from days ago from polluting a recent bucket
    // (events table can be back-filled by replay tools).
    const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    insertEvent(1, "LINE_CROSSING_B_TO_A", now_s - 10 * 60, lineCrossingMeta(10));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    EXPECT_EQ(countBuckets(/*camera*/1, /*source*/10), 0);
}

// ── /summary SQL contract ──────────────────────────────────────────────────

TEST_F(CounterIntegration, SummarySqlAggregatesAcrossMultipleBucketsAndLines) {
    // Three events across two minutes + two lines on one camera. The
    // /summary endpoint's SUM should see the total regardless of how the
    // aggregator decomposed it into buckets — this catches a column-name
    // or filter-clause regression where the read SQL drifts from the
    // write SQL but both still execute "successfully".
    const int64_t base = wellInsideWindow();
    const int64_t m1 = (base / 60) * 60;
    const int64_t m2 = m1 - 60;

    insertEvent(1, "LINE_CROSSING_B_TO_A", m1 + 10, lineCrossingMeta(10));
    insertEvent(1, "LINE_CROSSING_B_TO_A", m1 + 30, lineCrossingMeta(11));
    insertEvent(1, "LINE_CROSSING_A_TO_B", m2 + 15, lineCrossingMeta(10));

    vms::core::CounterBucketAggregator::getInstance().runSweepNow();

    // 3 buckets expected: (cam=1, line=10, m1), (cam=1, line=11, m1),
    // (cam=1, line=10, m2). The /summary SUM across them = in:2, out:1.
    EXPECT_EQ(countBuckets(/*camera*/1), 3);
    const auto [in_c, out_c] = summarySumForCamera(1, m2 - 60, m1 + 600);
    EXPECT_EQ(in_c,  2);
    EXPECT_EQ(out_c, 1);
}

} // namespace

int main(int argc, char** argv) {
    // Qt6 requires a QCoreApplication for plugin loading — without it, the
    // QSQLITE driver fails to resolve and QSqlDatabase::open() SEH-crashes
    // dereferencing a NULL driver pointer. App stays alive for the process
    // lifetime so all tests share the same Qt context.
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
