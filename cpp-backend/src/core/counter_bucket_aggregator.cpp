// ==============================================================
// File: src/core/counter_bucket_aggregator.cpp
// ==============================================================

#include "core/counter_bucket_aggregator.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "database/db_manager.h"
#include "utils/config.h"
#include "utils/logger.h"

namespace vms::core {

namespace {

constexpr int kMinIntervalSeconds   = 10;
constexpr int kMaxIntervalSeconds   = 600;
constexpr int kMinLookbackMinutes   = 1;
constexpr int kMaxLookbackMinutes   = 60;
constexpr int kBootstrapDelayMs     = 5000;
constexpr int kSweepWarnThresholdMs = 30000;

int readIntEnv(const char* key, int def, int lo, int hi) {
    const char* e = std::getenv(key);
    if (!e || !*e) return def;
    int v = std::atoi(e);
    return std::clamp(v, lo, hi);
}

// Composite key for bucket aggregation.
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

} // namespace

CounterBucketAggregator& CounterBucketAggregator::getInstance() {
    static CounterBucketAggregator inst;
    return inst;
}

CounterBucketAggregator::CounterBucketAggregator()
    : interval_seconds_(readIntEnv("VMS_COUNTER_BUCKET_INTERVAL_S", 60,
                                   kMinIntervalSeconds, kMaxIntervalSeconds)),
      lookback_minutes_(readIntEnv("VMS_COUNTER_BUCKET_LOOKBACK_MIN", 5,
                                   kMinLookbackMinutes, kMaxLookbackMinutes)),
      lateness_margin_s_(60) {}

CounterBucketAggregator::~CounterBucketAggregator() {
    stop();
}

void CounterBucketAggregator::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return; // already running
    stop_flag_.store(false, std::memory_order_release);
    worker_ = std::thread([this] { workerLoop(); });
    LOG_INFO("CounterBucketAggregator: started (interval={}s, lookback={}min)",
             interval_seconds_, lookback_minutes_);
}

void CounterBucketAggregator::stop() {
    if (!started_.load(std::memory_order_acquire)) return;
    stop_flag_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    started_.store(false, std::memory_order_release);
    LOG_INFO("CounterBucketAggregator: stopped (sweeps={}, upserted={})",
             sweeps_.load(), upserted_.load());
}

void CounterBucketAggregator::workerLoop() {
    // Bootstrap pause — let DB warm up, avoid racing with init.
    {
        std::unique_lock<std::mutex> lk(cv_mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(kBootstrapDelayMs),
                     [this] { return stop_flag_.load(std::memory_order_acquire); });
    }

    while (!stop_flag_.load(std::memory_order_acquire)) {
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        try {
            sweepOnce(now_s);
        } catch (const std::exception& e) {
            LOG_WARN("CounterBucketAggregator: sweep exception: {}", e.what());
        } catch (...) {
            LOG_WARN("CounterBucketAggregator: sweep unknown exception");
        }

        std::unique_lock<std::mutex> lk(cv_mu_);
        cv_.wait_for(lk, std::chrono::seconds(interval_seconds_),
                     [this] { return stop_flag_.load(std::memory_order_acquire); });
    }
}

void CounterBucketAggregator::runSweepNow() {
    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sweepOnce(now_s);
}

void CounterBucketAggregator::sweepOnce(int64_t now_s) {
    const auto t_start = std::chrono::steady_clock::now();

    // Window: [now_min - lookback, now_min - lateness_margin)
    // Aligning to minute boundaries so groupBy is exact.
    const int64_t now_minute   = (now_s / 60) * 60;
    const int64_t window_end_s = now_minute - lateness_margin_s_;
    const int64_t window_beg_s = now_minute - static_cast<int64_t>(lookback_minutes_) * 60;
    if (window_end_s <= window_beg_s) {
        // Lookback shorter than lateness margin — nothing to do this tick.
        sweeps_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto db = vms::database::DbManager::getInstance().getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_WARN("CounterBucketAggregator: DB unavailable, skipping sweep");
        sweeps_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    QSqlQuery sel(db);
    sel.prepare(
        "SELECT camera_id, event_type, timestamp, metadata_json "
        "FROM events "
        "WHERE event_type IN ('LINE_CROSSING_A_TO_B', 'LINE_CROSSING_B_TO_A') "
        "  AND timestamp >= ? AND timestamp < ?");
    sel.bindValue(0, static_cast<qlonglong>(window_beg_s));
    sel.bindValue(1, static_cast<qlonglong>(window_end_s));
    if (!sel.exec()) {
        LOG_WARN("CounterBucketAggregator: events query failed: {}",
                 sel.lastError().text().toStdString());
        sweeps_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Aggregate in C++ — driver-agnostic JSON parsing.
    std::unordered_map<BucketKey, BucketCounts, BucketKeyHash> agg;
    while (sel.next()) {
        const int     camera_id = sel.value(0).toInt();
        const QString etype     = sel.value(1).toString();
        const int64_t ts_s      = sel.value(2).toLongLong();
        const QString meta_q    = sel.value(3).toString();
        const std::string meta  = meta_q.toStdString();
        if (meta.empty()) continue;

        int line_id = -1;
        try {
            auto j = nlohmann::json::parse(meta);
            if (j.contains("line_id") && j["line_id"].is_number_integer()) {
                line_id = j["line_id"].get<int>();
            }
        } catch (...) {
            continue; // malformed → skip
        }
        if (line_id <= 0) continue;

        BucketKey k{camera_id, line_id, (ts_s / 60) * 60};
        auto& c = agg[k];
        if (etype == QStringLiteral("LINE_CROSSING_B_TO_A")) {
            c.in_count++;
        } else {
            c.out_count++;
        }
    }

    if (agg.empty()) {
        sweeps_.fetch_add(1, std::memory_order_relaxed);
        const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        last_sweep_ms_.store(static_cast<std::uint64_t>(dt), std::memory_order_relaxed);
        return;
    }

    // ON CONFLICT works in both SQLite (3.24+) and PostgreSQL with the same
    // syntax, so we don't branch on driver here.
    const std::string upsert_sql =
        "INSERT INTO counter_buckets_1m "
        "  (camera_id, source_kind, source_id, ts_minute, in_count, out_count) "
        "VALUES (?, 'line', ?, ?, ?, ?) "
        "ON CONFLICT(camera_id, source_kind, source_id, ts_minute) "
        "DO UPDATE SET in_count = excluded.in_count, "
        "              out_count = excluded.out_count";

    if (!db.transaction()) {
        LOG_WARN("CounterBucketAggregator: failed to begin transaction: {}",
                 db.lastError().text().toStdString());
    }

    QSqlQuery up(db);
    if (!up.prepare(QString::fromStdString(upsert_sql))) {
        LOG_WARN("CounterBucketAggregator: prepare failed: {}",
                 up.lastError().text().toStdString());
        db.rollback();
        sweeps_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::uint64_t n_upserted = 0;
    for (const auto& [k, c] : agg) {
        up.bindValue(0, k.camera_id);
        up.bindValue(1, k.line_id);
        up.bindValue(2, static_cast<qlonglong>(k.ts_minute));
        up.bindValue(3, c.in_count);
        up.bindValue(4, c.out_count);
        if (!up.exec()) {
            LOG_WARN("CounterBucketAggregator: upsert failed (cam={}, line={}, ts={}): {}",
                     k.camera_id, k.line_id, k.ts_minute,
                     up.lastError().text().toStdString());
            continue;
        }
        n_upserted++;
    }

    if (!db.commit()) {
        LOG_WARN("CounterBucketAggregator: commit failed: {}",
                 db.lastError().text().toStdString());
        db.rollback();
    }

    upserted_.fetch_add(n_upserted, std::memory_order_relaxed);
    sweeps_.fetch_add(1, std::memory_order_relaxed);

    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();
    last_sweep_ms_.store(static_cast<std::uint64_t>(dt), std::memory_order_relaxed);

    if (dt > kSweepWarnThresholdMs) {
        LOG_WARN("CounterBucketAggregator: slow sweep ({} ms, {} buckets)", dt, n_upserted);
    } else {
        LOG_DEBUG("CounterBucketAggregator: sweep OK ({} ms, {} buckets)", dt, n_upserted);
    }
}

}  // namespace vms::core
