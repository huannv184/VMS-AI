// ==============================================================
// File: tests/test_counter_bucket.cpp
// Inline reproduction of CounterBucketAggregator's group-by logic
// (mirrors the aggregation loop in src/core/counter_bucket_aggregator.cpp).
// Verifies direction → in/out mapping, ts_minute floor, skip rules
// for malformed metadata, and bucket merging.
//
// No DB / no link against backend code.
// ==============================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

struct EventRow {
    int         camera_id;
    std::string event_type;        // "LINE_CROSSING_A_TO_B" | "LINE_CROSSING_B_TO_A"
    int64_t     timestamp_s;       // epoch seconds
    std::string metadata_json;     // contains line_id
};

struct BucketKey {
    int     camera_id;
    int     line_id;
    int64_t ts_minute;
    bool operator==(const BucketKey& o) const noexcept {
        return camera_id == o.camera_id && line_id == o.line_id && ts_minute == o.ts_minute;
    }
};
struct BucketKeyHash {
    std::size_t operator()(const BucketKey& k) const noexcept {
        std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.camera_id));
        h = h * 1315423911u + static_cast<std::uint32_t>(k.line_id);
        h = h * 1315423911u + static_cast<std::uint64_t>(k.ts_minute);
        return static_cast<std::size_t>(h);
    }
};
struct BucketCounts {
    int in_count  = 0;
    int out_count = 0;
};

using BucketMap = std::unordered_map<BucketKey, BucketCounts, BucketKeyHash>;

// Mirror of the aggregation in CounterBucketAggregator::sweepOnce.
// Convention: B_TO_A → in_count, A_TO_B → out_count.
BucketMap aggregate(const std::vector<EventRow>& rows) {
    BucketMap agg;
    for (const auto& r : rows) {
        if (r.metadata_json.empty()) continue;

        int line_id = -1;
        try {
            auto j = nlohmann::json::parse(r.metadata_json);
            if (j.contains("line_id") && j["line_id"].is_number_integer()) {
                line_id = j["line_id"].get<int>();
            }
        } catch (...) {
            continue;
        }
        if (line_id <= 0) continue;

        BucketKey k{r.camera_id, line_id, (r.timestamp_s / 60) * 60};
        auto& c = agg[k];
        if (r.event_type == "LINE_CROSSING_B_TO_A") {
            c.in_count++;
        } else if (r.event_type == "LINE_CROSSING_A_TO_B") {
            c.out_count++;
        }
    }
    return agg;
}

EventRow makeRow(int cam, int line_id, const std::string& etype, int64_t ts_s) {
    nlohmann::json m = {{"line_id", line_id}};
    return EventRow{cam, etype, ts_s, m.dump()};
}

} // namespace

TEST(CounterBucket, AtoBContributesToOutCount) {
    auto m = aggregate({makeRow(1, 10, "LINE_CROSSING_A_TO_B", 1700000060)});
    ASSERT_EQ(m.size(), 1u);
    auto it = m.begin();
    EXPECT_EQ(it->second.in_count,  0);
    EXPECT_EQ(it->second.out_count, 1);
}

TEST(CounterBucket, BtoAContributesToInCount) {
    auto m = aggregate({makeRow(1, 10, "LINE_CROSSING_B_TO_A", 1700000060)});
    ASSERT_EQ(m.size(), 1u);
    auto it = m.begin();
    EXPECT_EQ(it->second.in_count,  1);
    EXPECT_EQ(it->second.out_count, 0);
}

TEST(CounterBucket, MultipleEventsSameKeyMerge) {
    auto m = aggregate({
        makeRow(1, 10, "LINE_CROSSING_A_TO_B", 1700000060),
        makeRow(1, 10, "LINE_CROSSING_A_TO_B", 1700000070),  // same minute
        makeRow(1, 10, "LINE_CROSSING_B_TO_A", 1700000080),
    });
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m.begin()->second.in_count,  1);
    EXPECT_EQ(m.begin()->second.out_count, 2);
}

TEST(CounterBucket, DifferentMinutesAreDifferentBuckets) {
    auto m = aggregate({
        makeRow(1, 10, "LINE_CROSSING_A_TO_B", 1700000060),  // minute 28333334
        makeRow(1, 10, "LINE_CROSSING_A_TO_B", 1700000125),  // minute 28333335 (next minute)
    });
    EXPECT_EQ(m.size(), 2u);
}

TEST(CounterBucket, DifferentCamerasAreDifferentBuckets) {
    auto m = aggregate({
        makeRow(1, 10, "LINE_CROSSING_A_TO_B", 1700000060),
        makeRow(2, 10, "LINE_CROSSING_A_TO_B", 1700000060),
    });
    EXPECT_EQ(m.size(), 2u);
}

TEST(CounterBucket, DifferentLinesAreDifferentBuckets) {
    auto m = aggregate({
        makeRow(1, 10, "LINE_CROSSING_A_TO_B", 1700000060),
        makeRow(1, 11, "LINE_CROSSING_A_TO_B", 1700000060),
    });
    EXPECT_EQ(m.size(), 2u);
}

TEST(CounterBucket, TsMinuteFloorsCorrectly) {
    // ts=1700000099 → minute=floor(1700000099/60)*60 = 1700000040.
    auto m = aggregate({makeRow(1, 10, "LINE_CROSSING_A_TO_B", 1700000099)});
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m.begin()->first.ts_minute, 1700000040);
}

TEST(CounterBucket, MalformedJsonDropped) {
    auto m = aggregate({EventRow{1, "LINE_CROSSING_A_TO_B", 1700000060, "{not json"}});
    EXPECT_TRUE(m.empty());
}

TEST(CounterBucket, EmptyMetadataDropped) {
    auto m = aggregate({EventRow{1, "LINE_CROSSING_A_TO_B", 1700000060, ""}});
    EXPECT_TRUE(m.empty());
}

TEST(CounterBucket, MissingLineIdDropped) {
    auto m = aggregate({EventRow{1, "LINE_CROSSING_A_TO_B", 1700000060, "{}"}});
    EXPECT_TRUE(m.empty());
}

TEST(CounterBucket, NonIntegerLineIdDropped) {
    auto m = aggregate({EventRow{1, "LINE_CROSSING_A_TO_B", 1700000060,
                                  R"({"line_id":"abc"})"}});
    EXPECT_TRUE(m.empty());
}

TEST(CounterBucket, NegativeLineIdDropped) {
    auto m = aggregate({EventRow{1, "LINE_CROSSING_A_TO_B", 1700000060,
                                  R"({"line_id":-1})"}});
    EXPECT_TRUE(m.empty());
}

TEST(CounterBucket, UnknownEventTypeNoCount) {
    // Defensive — sweep filters at SQL layer, but if a stray row leaks in
    // the C++ side must not crash or attribute it.
    auto rows = std::vector<EventRow>{
        makeRow(1, 10, "FACE_RECOGNIZED", 1700000060)
    };
    auto m = aggregate(rows);
    // Bucket gets created with line_id but counts stay zero.
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m.begin()->second.in_count,  0);
    EXPECT_EQ(m.begin()->second.out_count, 0);
}
