// Unit tests for the lock-contention instrumentation primitive
// (include/core/lock_monitor.h). Exercises WriterGuard / ReaderGuard
// against a vanilla shared_mutex so the test target stays free of
// PipelineStateStore's heavy dependency chain (Qt, nlohmann_json,
// inference types). PSS's wiring is exercised at compile time + by
// the existing ctest targets that touch PSS through camera lifecycle.

#include "core/lock_monitor.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace {

using vms::core::LockStats;
using vms::core::ReaderGuard;
using vms::core::WriterGuard;
using vms::core::snapshotLockStats;

// Acquiring an uncontended writer lock increments writer_acquisitions
// but should NOT mark the acquisition as contended.
TEST(LockMonitor, WriterUncontendedDoesNotMarkContended) {
    std::shared_mutex m;
    LockStats stats;

    {
        WriterGuard g(m, stats);
    }

    auto s = snapshotLockStats(stats);
    EXPECT_EQ(s.writer_acquisitions, 1u);
    EXPECT_EQ(s.writer_contended, 0u);
    EXPECT_EQ(s.writer_wait_ns_total, 0u);
    EXPECT_EQ(s.writer_wait_ns_max, 0u);
}

TEST(LockMonitor, ReaderUncontendedDoesNotMarkContended) {
    std::shared_mutex m;
    LockStats stats;

    {
        ReaderGuard g(m, stats);
    }

    auto s = snapshotLockStats(stats);
    EXPECT_EQ(s.reader_acquisitions, 1u);
    EXPECT_EQ(s.reader_contended, 0u);
    EXPECT_EQ(s.reader_wait_ns_total, 0u);
    EXPECT_EQ(s.reader_wait_ns_max, 0u);
}

TEST(LockMonitor, MultipleReadersDoNotContend) {
    std::shared_mutex m;
    LockStats stats;

    {
        ReaderGuard a(m, stats);
        ReaderGuard b(m, stats);
        ReaderGuard c(m, stats);
    }

    auto s = snapshotLockStats(stats);
    EXPECT_EQ(s.reader_acquisitions, 3u);
    EXPECT_EQ(s.reader_contended, 0u);
}

// A reader trying to acquire while a writer holds the lock must
// block. The instrumented branch should mark this as contended and
// record a wait time greater than zero.
TEST(LockMonitor, ReaderContendsWithHeldWriter) {
    std::shared_mutex m;
    LockStats stats;
    std::atomic<bool> writer_holding{false};
    std::atomic<bool> reader_done{false};

    std::thread writer([&]() {
        WriterGuard g(m, stats);
        writer_holding.store(true, std::memory_order_release);
        // Hold long enough that the reader's try_lock_shared is guaranteed
        // to fail and we capture a measurable wait.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    while (!writer_holding.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread reader([&]() {
        ReaderGuard g(m, stats);
        reader_done.store(true, std::memory_order_release);
    });

    writer.join();
    reader.join();
    EXPECT_TRUE(reader_done.load());

    auto s = snapshotLockStats(stats);
    EXPECT_EQ(s.writer_acquisitions, 1u);
    EXPECT_EQ(s.reader_acquisitions, 1u);
    EXPECT_GE(s.reader_contended, 1u);
    EXPECT_GT(s.reader_wait_ns_total, 0u);
    EXPECT_GT(s.reader_wait_ns_max, 0u);
}

// A writer trying to acquire while a reader holds the shared lock
// must block. Same instrumentation expectation on the writer side.
TEST(LockMonitor, WriterContendsWithHeldReader) {
    std::shared_mutex m;
    LockStats stats;
    std::atomic<bool> reader_holding{false};

    std::thread reader([&]() {
        ReaderGuard g(m, stats);
        reader_holding.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    while (!reader_holding.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread writer([&]() {
        WriterGuard g(m, stats);
    });

    reader.join();
    writer.join();

    auto s = snapshotLockStats(stats);
    EXPECT_EQ(s.reader_acquisitions, 1u);
    EXPECT_EQ(s.writer_acquisitions, 1u);
    EXPECT_GE(s.writer_contended, 1u);
    EXPECT_GT(s.writer_wait_ns_total, 0u);
    EXPECT_GT(s.writer_wait_ns_max, 0u);
}

// max_wait_ns_ must hold the largest single observation, not the
// most recent one. CAS-update path is the load-bearing piece here.
TEST(LockMonitor, MaxWaitMonotonicallyTracksLargestSample) {
    std::shared_mutex m;
    LockStats stats;

    auto block_writer_for = [&](std::chrono::milliseconds dur) {
        std::atomic<bool> holding{false};
        std::thread w([&]() {
            WriterGuard g(m, stats);
            holding.store(true, std::memory_order_release);
            std::this_thread::sleep_for(dur);
        });
        while (!holding.load(std::memory_order_acquire)) std::this_thread::yield();
        std::thread r([&]() { ReaderGuard g(m, stats); });
        w.join();
        r.join();
    };

    block_writer_for(std::chrono::milliseconds(40));
    auto after_big = snapshotLockStats(stats).reader_wait_ns_max;
    EXPECT_GT(after_big, 0u);

    // Subsequent shorter blocks must NOT lower the recorded max.
    block_writer_for(std::chrono::milliseconds(5));
    auto after_small = snapshotLockStats(stats).reader_wait_ns_max;
    EXPECT_GE(after_small, after_big);
}

// Stress test: aggregate counters under concurrent reader+writer
// pressure. Verifies (a) acquisitions match work performed exactly
// and (b) contended <= acquisitions invariant holds. We intentionally
// do NOT require contended > 0 here — on MSVC SRWLOCK the try_lock
// fast path is fast enough that empty-body loops may never collide
// even under 6 threads. The dedicated *ContendsWith* tests above
// already pin the instrumented-slow-path behaviour.
TEST(LockMonitor, StressMonotonicAndAccountsAllAcquisitions) {
    std::shared_mutex m;
    LockStats stats;

    constexpr int kReaders        = 4;
    constexpr int kWriters        = 2;
    constexpr int kOpsPerThread   = 500;

    auto reader_fn = [&]() {
        for (int i = 0; i < kOpsPerThread; ++i) {
            ReaderGuard g(m, stats);
        }
    };
    auto writer_fn = [&]() {
        for (int i = 0; i < kOpsPerThread; ++i) {
            WriterGuard g(m, stats);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kReaders + kWriters);
    for (int i = 0; i < kReaders; ++i) threads.emplace_back(reader_fn);
    for (int i = 0; i < kWriters; ++i) threads.emplace_back(writer_fn);
    for (auto& t : threads) t.join();

    auto s = snapshotLockStats(stats);
    EXPECT_EQ(s.reader_acquisitions, static_cast<uint64_t>(kReaders * kOpsPerThread));
    EXPECT_EQ(s.writer_acquisitions, static_cast<uint64_t>(kWriters * kOpsPerThread));
    EXPECT_LE(s.reader_contended, s.reader_acquisitions);
    EXPECT_LE(s.writer_contended, s.writer_acquisitions);
}

} // namespace
