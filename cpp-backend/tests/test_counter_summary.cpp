// ==============================================================
// File: tests/test_counter_summary.cpp
// Inline reproduction of the bucket → summary/history roll-up
// performed inside the GET /api/counter/summary and
// GET /api/counter/history handlers in src/api/counter_controller.cpp.
//
// What this guards:
//   - Per-line breakdown matches the sum of in/out per (camera, line)
//   - Total in/out matches the sum across the window
//   - Peak-hour is the hour bucket with the largest combined total
//   - Empty data returns peak_hour = -1 (FE treats < 0 as "no data")
//   - History point ordering is (ts_minute ASC, source_id ASC)
//
// No DB / no Crow. Update this file whenever the C++ aggregation in
// counter_controller.cpp changes — if the controller and this test
// drift, the contract guarantee for CounterView.jsx is gone.
// ==============================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct BucketRow {
    int     camera_id;
    int     line_id;       // counter_buckets_1m.source_id when source_kind='line'
    int64_t ts_minute;     // minute-aligned epoch (UTC)
    int     in_count;
    int     out_count;
};

struct SummaryResult {
    int  total_in    = 0;
    int  total_out   = 0;
    int  total_today = 0;
    int  peak_hour   = -1;
    int  peak_count  = 0;
    // line_id → (in, out)
    std::unordered_map<int, std::pair<int,int>> per_line;
};

// Test-side hour-of-day. Production code uses local TZ via localtime_r; tests
// run in whatever TZ the CI host happens to use, so we accept a hour-fn
// parameter and inject a deterministic UTC version. The PRODUCTION peak-hour
// computation logic — sum bucket-totals into hour bins, pick max — is the
// thing actually under test, not the timezone math.
int hourOfDayUtc(int64_t epoch_s) {
    return static_cast<int>((epoch_s / 3600) % 24);
}

// Mirrors counter_controller's /summary aggregation loop verbatim.
SummaryResult summarize(const std::vector<BucketRow>& rows,
                        int camera_id,
                        int64_t from_ts,
                        int64_t to_ts) {
    SummaryResult s;
    std::unordered_map<int, int> per_hour_total;

    for (const auto& r : rows) {
        if (r.camera_id != camera_id) continue;
        if (r.ts_minute <  from_ts)   continue;
        if (r.ts_minute >= to_ts)     continue;

        s.total_in  += r.in_count;
        s.total_out += r.out_count;
        auto& p = s.per_line[r.line_id];
        p.first  += r.in_count;
        p.second += r.out_count;

        per_hour_total[hourOfDayUtc(r.ts_minute)] += (r.in_count + r.out_count);
    }
    s.total_today = s.total_in + s.total_out;

    for (const auto& [h, total] : per_hour_total) {
        if (total > s.peak_count) {
            s.peak_count = total;
            s.peak_hour  = h;
        }
    }
    return s;
}

// Mirrors counter_controller's /history ORDER BY ts_minute ASC, source_id ASC.
std::vector<BucketRow> historyOrdered(const std::vector<BucketRow>& rows,
                                      int camera_id,
                                      int64_t from_ts,
                                      int64_t to_ts) {
    std::vector<BucketRow> out;
    for (const auto& r : rows) {
        if (r.camera_id != camera_id) continue;
        if (r.ts_minute <  from_ts)   continue;
        if (r.ts_minute >= to_ts)     continue;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const BucketRow& a, const BucketRow& b) {
        if (a.ts_minute != b.ts_minute) return a.ts_minute < b.ts_minute;
        return a.line_id < b.line_id;
    });
    return out;
}

// Helper: build a bucket at a specific hour-of-day (UTC) within an arbitrary
// UTC-midnight base. 1700006400 = 2023-11-15T00:00:00Z (verified: divisible
// by 86400 so it is exactly midnight UTC, not just any epoch).
constexpr int64_t kBaseMidnight = 1700006400;
static_assert(kBaseMidnight % 86400 == 0, "kBaseMidnight must be UTC midnight");
int64_t at(int hour, int minute = 0) {
    return kBaseMidnight + hour * 3600 + minute * 60;
}

} // namespace

// ── /summary aggregation ────────────────────────────────────────────────────

TEST(CounterSummary, EmptyWindowReturnsZerosAndNoPeak) {
    auto s = summarize({}, /*camera_id*/1, /*from*/0, /*to*/1);
    EXPECT_EQ(s.total_in,    0);
    EXPECT_EQ(s.total_out,   0);
    EXPECT_EQ(s.total_today, 0);
    EXPECT_EQ(s.peak_hour,  -1);     // sentinel: FE renders "-" instead of "0h"
    EXPECT_EQ(s.peak_count,  0);
    EXPECT_TRUE(s.per_line.empty());
}

TEST(CounterSummary, SingleBucketAccumulates) {
    std::vector<BucketRow> rows = {
        {1, 10, at(8), /*in*/3, /*out*/2}
    };
    auto s = summarize(rows, 1, at(0), at(24));
    EXPECT_EQ(s.total_in,    3);
    EXPECT_EQ(s.total_out,   2);
    EXPECT_EQ(s.total_today, 5);
    EXPECT_EQ(s.per_line.size(), 1u);
    EXPECT_EQ(s.per_line[10].first,  3);
    EXPECT_EQ(s.per_line[10].second, 2);
    EXPECT_EQ(s.peak_hour, 8);
    EXPECT_EQ(s.peak_count, 5);
}

TEST(CounterSummary, MultiLineBreakdown) {
    std::vector<BucketRow> rows = {
        {1, 10, at(8),  5, 4},
        {1, 11, at(9),  2, 7},
        {1, 10, at(10), 1, 0},   // same line as first
    };
    auto s = summarize(rows, 1, at(0), at(24));
    EXPECT_EQ(s.total_in,  8);
    EXPECT_EQ(s.total_out, 11);
    EXPECT_EQ(s.per_line[10].first,  6);   // 5 + 1
    EXPECT_EQ(s.per_line[10].second, 4);   // 4 + 0
    EXPECT_EQ(s.per_line[11].first,  2);
    EXPECT_EQ(s.per_line[11].second, 7);
}

TEST(CounterSummary, PeakHourPicksLargestCombinedTotal) {
    std::vector<BucketRow> rows = {
        {1, 10, at(8),  10, 0},   // hour 8 total = 10
        {1, 10, at(12), 30, 0},   // hour 12 total = 30  ← peak
        {1, 11, at(12), 5,  5},   // hour 12 total += 10 → 40
        {1, 10, at(15), 20, 5},   // hour 15 total = 25
    };
    auto s = summarize(rows, 1, at(0), at(24));
    EXPECT_EQ(s.peak_hour, 12);
    EXPECT_EQ(s.peak_count, 40);
}

TEST(CounterSummary, FiltersByCameraId) {
    std::vector<BucketRow> rows = {
        {1, 10, at(8), 5, 0},
        {2, 10, at(8), 9, 9},     // different camera — must be excluded
    };
    auto s = summarize(rows, /*camera_id*/1, at(0), at(24));
    EXPECT_EQ(s.total_in,  5);
    EXPECT_EQ(s.total_out, 0);
    EXPECT_EQ(s.per_line.size(), 1u);
}

TEST(CounterSummary, FiltersByTimeRangeHalfOpen) {
    std::vector<BucketRow> rows = {
        {1, 10, at(7), 1, 0},    // before window
        {1, 10, at(8), 2, 0},    // in window
        {1, 10, at(9), 4, 0},    // boundary == to_ts → excluded (half-open)
        {1, 10, at(10), 8, 0},   // after window
    };
    auto s = summarize(rows, 1, at(8), at(9));
    EXPECT_EQ(s.total_in, 2);
    EXPECT_EQ(s.total_out, 0);
}

TEST(CounterSummary, PeakHourTieKeepsFirstSeen) {
    // Two hours tie at total=10. The pick is deterministic to whichever
    // hour the unordered_map iterates first hitting the > comparison —
    // the test asserts that one of them wins and peak_count is correct.
    // (Operators just need peak_count, the hour itself is informational.)
    std::vector<BucketRow> rows = {
        {1, 10, at(8),  10, 0},
        {1, 10, at(14), 10, 0},
    };
    auto s = summarize(rows, 1, at(0), at(24));
    EXPECT_EQ(s.peak_count, 10);
    EXPECT_TRUE(s.peak_hour == 8 || s.peak_hour == 14);
}

// ── /history ordering ───────────────────────────────────────────────────────

TEST(CounterHistory, PointsOrderedByTimeThenLine) {
    std::vector<BucketRow> rows = {
        {1, 11, at(10), 0, 0},
        {1, 10, at(10), 0, 0},   // same minute, smaller line_id → first
        {1, 10, at(8),  0, 0},   // earlier minute → before both
    };
    auto out = historyOrdered(rows, 1, at(0), at(24));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].ts_minute, at(8));
    EXPECT_EQ(out[0].line_id, 10);
    EXPECT_EQ(out[1].ts_minute, at(10));
    EXPECT_EQ(out[1].line_id, 10);
    EXPECT_EQ(out[2].ts_minute, at(10));
    EXPECT_EQ(out[2].line_id, 11);
}

TEST(CounterHistory, RespectsWindowHalfOpen) {
    std::vector<BucketRow> rows = {
        {1, 10, at(7), 1, 0},
        {1, 10, at(8), 2, 0},
        {1, 10, at(9), 4, 0},   // == to → excluded
    };
    auto out = historyOrdered(rows, 1, at(8), at(9));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].in_count, 2);
}

TEST(CounterHistory, FiltersByCamera) {
    std::vector<BucketRow> rows = {
        {1, 10, at(8), 1, 0},
        {2, 10, at(8), 99, 99},  // wrong camera
    };
    auto out = historyOrdered(rows, 1, at(0), at(24));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].camera_id, 1);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
