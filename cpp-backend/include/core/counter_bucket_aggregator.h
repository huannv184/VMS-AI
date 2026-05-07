// ==============================================================
// File: include/core/counter_bucket_aggregator.h
// Background aggregator that rolls LINE_CROSSING_* events into
// the counter_buckets_1m table for fast timeseries queries.
//
// Algorithm:
//   Worker thread ticks every 60s. Each tick re-aggregates the
//   most recent LOOKBACK_MINUTES of fully-elapsed minutes (skipping
//   the current minute, which is still being filled). Late events
//   landing within that window get re-counted via UPSERT — the
//   bucket count is set absolutely (not incremented), so re-runs
//   are idempotent.
//
//   Convention:
//     direction_code = "b_to_a"  → in_count   (going TO side A)
//     direction_code = "a_to_b"  → out_count  (going TO side B)
//   This matches counter_controller's default labels (A="in", B="out").
//   Customers using non-in/out labels can still query raw events.
//
// Concurrency:
//   - Single dedicated worker thread; reads events table with a
//     range-scan over the timestamp index, writes only to
//     counter_buckets_1m (UNIQUE constraint backstop).
//   - No locks shared with hot path. Stop() joins on shutdown.
//
// Failure modes:
//   - DB unavailable        → log warn, retry next tick
//   - Bad metadata_json     → drop that row, continue
//   - Sweep overrun (>60s)  → next tick simply replaces the same
//                              bucket with the latest count
// ==============================================================

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace vms::core {

class CounterBucketAggregator {
public:
    static CounterBucketAggregator& getInstance();

    // Lifecycle (idempotent).
    void start();
    void stop();

    // Health / introspection.
    std::uint64_t totalSweeps() const   { return sweeps_.load(std::memory_order_relaxed); }
    std::uint64_t totalUpserted() const { return upserted_.load(std::memory_order_relaxed); }
    std::uint64_t lastSweepMs() const   { return last_sweep_ms_.load(std::memory_order_relaxed); }

    // Force an immediate sweep — for tests and the ops escape hatch.
    // Safe to call from any thread; runs synchronously on the caller.
    void runSweepNow();

    // Tunables (env): VMS_COUNTER_BUCKET_INTERVAL_S, VMS_COUNTER_BUCKET_LOOKBACK_MIN.
    // Min interval: 10s (guard runaway loops); Max lookback: 60min.

private:
    CounterBucketAggregator();
    ~CounterBucketAggregator();
    CounterBucketAggregator(const CounterBucketAggregator&) = delete;
    CounterBucketAggregator& operator=(const CounterBucketAggregator&) = delete;

    void workerLoop();
    void sweepOnce(int64_t now_s);

    int  interval_seconds_;     // tick cadence, default 60
    int  lookback_minutes_;     // re-aggregate window, default 5
    int  lateness_margin_s_;    // skip "now" minute (60)

    std::atomic<bool>     started_{false};
    std::atomic<bool>     stop_flag_{false};
    std::thread           worker_;
    std::mutex            cv_mu_;
    std::condition_variable cv_;

    std::atomic<std::uint64_t> sweeps_{0};
    std::atomic<std::uint64_t> upserted_{0};
    std::atomic<std::uint64_t> last_sweep_ms_{0};
};

}  // namespace vms::core
