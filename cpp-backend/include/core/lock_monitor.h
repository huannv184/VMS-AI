#pragma once

// Lock-contention instrumentation for shared_mutex-protected hot paths.
//
// 2026-06-04 PSS measurement gate: per-camera mutex sharding has been
// "defer measure first" since the 2026-05-15 hot-path audit. This header
// supplies the measure. WriterGuard / ReaderGuard wrap acquisition with
// a try-lock-then-time-the-block pattern so uncontended paths pay only
// one extra atomic increment (~1ns on x86) and contended paths capture
// a chrono::now pair + four atomics (negligible vs. the mutex wait
// itself). Counters live in an injected LockStats so each protected
// data structure can own its own snapshot — same shape as the existing
// batch_writer / delivery / websocket counter blocks merged into
// /api/rules/stats.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <shared_mutex>

namespace vms::core {

struct LockStats {
    // Counts include both contended and uncontended acquisitions, so
    // contended / acquisitions gives the contention ratio directly.
    std::atomic<uint64_t> writer_acquisitions{0};
    std::atomic<uint64_t> writer_contended{0};
    std::atomic<uint64_t> writer_wait_ns_total{0};
    std::atomic<uint64_t> writer_wait_ns_max{0};
    std::atomic<uint64_t> reader_acquisitions{0};
    std::atomic<uint64_t> reader_contended{0};
    std::atomic<uint64_t> reader_wait_ns_total{0};
    std::atomic<uint64_t> reader_wait_ns_max{0};
};

struct LockStatsSnapshot {
    uint64_t writer_acquisitions{0};
    uint64_t writer_contended{0};
    uint64_t writer_wait_ns_total{0};
    uint64_t writer_wait_ns_max{0};
    uint64_t reader_acquisitions{0};
    uint64_t reader_contended{0};
    uint64_t reader_wait_ns_total{0};
    uint64_t reader_wait_ns_max{0};
};

inline LockStatsSnapshot snapshotLockStats(const LockStats& s) {
    // memory_order_relaxed: counters are monotonic operational stats,
    // not used to synchronize protected data. A torn read across the
    // 8 fields is benign — operators consume aggregate ratios.
    return LockStatsSnapshot{
        s.writer_acquisitions.load(std::memory_order_relaxed),
        s.writer_contended.load(std::memory_order_relaxed),
        s.writer_wait_ns_total.load(std::memory_order_relaxed),
        s.writer_wait_ns_max.load(std::memory_order_relaxed),
        s.reader_acquisitions.load(std::memory_order_relaxed),
        s.reader_contended.load(std::memory_order_relaxed),
        s.reader_wait_ns_total.load(std::memory_order_relaxed),
        s.reader_wait_ns_max.load(std::memory_order_relaxed),
    };
}

namespace detail {

inline void updateMaxRelaxed(std::atomic<uint64_t>& dst, uint64_t candidate) {
    uint64_t prev = dst.load(std::memory_order_relaxed);
    while (candidate > prev &&
           !dst.compare_exchange_weak(prev, candidate,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
        // prev was reloaded by compare_exchange_weak; loop until either
        // the candidate stops being a new max or the CAS succeeds.
    }
}

} // namespace detail

// RAII helper: take exclusive ownership of a shared_mutex while
// instrumenting acquisition time. The try-lock fast path keeps
// uncontended acquisitions cheap (one syscall + one atomic) and only
// the slow path pays the chrono::now pair.
class WriterGuard {
public:
    WriterGuard(std::shared_mutex& m, LockStats& stats)
        : mutex_(m), stats_(stats) {
        if (mutex_.try_lock()) {
            stats_.writer_acquisitions.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        mutex_.lock();
        const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        const uint64_t wait_ns = wait > 0 ? static_cast<uint64_t>(wait) : 0;
        stats_.writer_acquisitions.fetch_add(1, std::memory_order_relaxed);
        stats_.writer_contended.fetch_add(1, std::memory_order_relaxed);
        stats_.writer_wait_ns_total.fetch_add(wait_ns, std::memory_order_relaxed);
        detail::updateMaxRelaxed(stats_.writer_wait_ns_max, wait_ns);
    }

    ~WriterGuard() { mutex_.unlock(); }

    WriterGuard(const WriterGuard&) = delete;
    WriterGuard& operator=(const WriterGuard&) = delete;
    WriterGuard(WriterGuard&&) = delete;
    WriterGuard& operator=(WriterGuard&&) = delete;

private:
    std::shared_mutex& mutex_;
    LockStats& stats_;
};

// RAII helper: take shared (reader) ownership with the same
// instrumentation pattern. try_lock_shared() is the fast path; only
// the contended branch pays the timing cost.
class ReaderGuard {
public:
    ReaderGuard(std::shared_mutex& m, LockStats& stats)
        : mutex_(m), stats_(stats) {
        if (mutex_.try_lock_shared()) {
            stats_.reader_acquisitions.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        mutex_.lock_shared();
        const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        const uint64_t wait_ns = wait > 0 ? static_cast<uint64_t>(wait) : 0;
        stats_.reader_acquisitions.fetch_add(1, std::memory_order_relaxed);
        stats_.reader_contended.fetch_add(1, std::memory_order_relaxed);
        stats_.reader_wait_ns_total.fetch_add(wait_ns, std::memory_order_relaxed);
        detail::updateMaxRelaxed(stats_.reader_wait_ns_max, wait_ns);
    }

    ~ReaderGuard() { mutex_.unlock_shared(); }

    ReaderGuard(const ReaderGuard&) = delete;
    ReaderGuard& operator=(const ReaderGuard&) = delete;
    ReaderGuard(ReaderGuard&&) = delete;
    ReaderGuard& operator=(ReaderGuard&&) = delete;

private:
    std::shared_mutex& mutex_;
    LockStats& stats_;
};

} // namespace vms::core
