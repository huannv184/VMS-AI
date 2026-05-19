#pragma once

#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <string>
#include <vector>
#include <map>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QThread>

#include "utils/config.h"
#include "database/models.h"

namespace vms {
namespace database {

/**
 * @brief Database manager using Qt6 QSqlDatabase
 * Singleton pattern with thread-local connections for safe concurrent access.
 *
 * Each thread obtains its own QSqlDatabase connection via getThreadConnection().
 * The primary connection is created during init() and used for table setup.
 * The batch writer thread maintains its own dedicated connection.
 */
class DbManager {
public:
    /**
     * @brief Get singleton instance
     */
    static DbManager& getInstance();

    /**
     * @brief Initialize database connection
     * @param config Database configuration from yaml
     * @return true if successful, false otherwise
     */
    bool init(const Config::DatabaseConfig& config);

    /**
     * @brief Close database connection and all thread-local connections
     */
    void close();

    /**
     * @brief Get a QSqlDatabase connection for the calling thread.
     * If no connection exists for this thread, one is created (cloned from primary).
     * @return QSqlDatabase for the calling thread
     */
    QSqlDatabase getThreadConnection();

    /**
     * @brief Execute SQL query using the calling thread's connection
     * @param sql SQL query string
     * @return true if successful, false otherwise
     */
    bool execute(const std::string& sql);

    /**
     * @brief Execute SQL query with parameters to prevent SQL injection
     * @param sql SQL query string with ? placeholders
     * @param params Vector of string parameters
     * @return true if successful, false otherwise
     */
    bool executeParameterized(const std::string& sql, const std::vector<std::string>& params);

    /**
     * @brief Begin transaction on the calling thread's connection
     */
    void beginTransaction();

    /**
     * @brief Commit transaction on the calling thread's connection
     */
    void commit();

    /**
     * @brief Rollback transaction on the calling thread's connection
     */
    void rollback();

    /**
     * @brief Get setting value
     * @param key Setting key
     * @param default_val Default value if key not found
     */
    std::string getSetting(const std::string& key, const std::string& default_val = "");

    /**
     * @brief Set setting value
     * @param key Setting key
     * @param value Setting value
     */
    bool setSetting(const std::string& key, const std::string& value);

    /**
     * @brief Get all settings
     * @return Map of all settings
     */
    std::map<std::string, std::string> getAllSettings();

    /**
     * @brief Enqueue an event for batched insertion.
     * Returns immediately; the batch writer thread flushes periodically.
     * @return true if event was accepted into the queue, false if rejected (shutdown/not initialized).
     */
    bool enqueueEvent(const Event& event);

    /**
     * @brief Check if database is initialized
     */
    bool isInitialized() const { return initialized_.load(std::memory_order_acquire); }

    /**
     * @brief Snapshot of event batch writer counters.
     *
     * Returned by batchWriterStats(). Exposed externally on
     * GET /api/rules/stats (admin RBAC) so operators can answer
     * "did we lose events during last night's burst?" — previously the
     * only signal was a throttled WARN log line.
     *
     * All counters are monotonic uint64_t atomic loads; the struct is a
     * point-in-time snapshot and may be inconsistent across fields for a
     * few ns but is wait-free w.r.t. the producer hot path.
     */
    struct BatchWriterStats {
        std::size_t  current_queue_depth{0};
        std::size_t  max_queue_size{0};
        std::size_t  batch_flush_size{0};
        std::uint64_t enqueued_total{0};
        std::uint64_t dropped_total{0};         // queue-full + not-accepting drops
        std::uint64_t flushed_total{0};         // successful row insertions
        std::uint64_t flush_failures_total{0};  // commit failures, transaction aborts
        std::uint64_t row_failures_total{0};    // per-row insert exec failures (poisoned events)
        std::uint64_t peak_queue_depth{0};
        bool running{false};
        bool accepting{false};
    };

    /// Wait-free read of the batch writer state. Safe to call from any
    /// HTTP handler thread.
    BatchWriterStats batchWriterStats() const;

    // Returns true when connected to PostgreSQL (vs SQLite).
    // Use to select SQL dialect for non-portable functions.
    bool isPostgres() const { return config_.driver == "postgresql"; }

    // Returns the SQL expression for current Unix epoch timestamp.
    // SQLite: strftime('%s','now')  PostgreSQL: EXTRACT(EPOCH FROM NOW())::BIGINT
    std::string sqlNowEpoch() const {
        return isPostgres() ? "EXTRACT(EPOCH FROM NOW())::BIGINT" : "strftime('%s','now')";
    }

    // Returns the SQL expression to get the last inserted row id.
    // SQLite: last_insert_rowid()  PostgreSQL: use RETURNING id clause instead.
    std::string sqlLastInsertId() const {
        return isPostgres() ? "0" /* unused — caller must use RETURNING */ : "last_insert_rowid()";
    }

private:
    DbManager() = default;
    ~DbManager();

    // Delete copy and move
    DbManager(const DbManager&) = delete;
    DbManager& operator=(const DbManager&) = delete;

    /**
     * @brief Initialize all database tables (DDL + migrations)
     */
    void initializeTables(QSqlDatabase& primary);

    /**
     * @brief Execute SQL on a specific connection without acquiring any external lock.
     * Used internally during init when the primary connection is already set up.
     */
    bool executeOnConnection(QSqlDatabase& db, const std::string& sql);

    /**
     * @brief Safely attempt an ALTER TABLE migration.
     * Ignores "duplicate column name" errors.
     */
    void safeAlterTable(QSqlDatabase& db, const std::string& sql, const std::string& column_name);

    /**
     * @brief Generate a unique connection name for the calling thread.
     */
    static QString threadConnectionName();

    // Primary connection name
    static constexpr const char* kPrimaryConnectionName = "vms_primary";

    // Database configuration
    Config::DatabaseConfig config_;

    // Initialization flag
    std::atomic<bool> initialized_{false};

    // 2026-05-19 transaction_mutex_ removed (was a global app-level
    // serializer). Per-thread QSqlDatabase via getThreadConnection()
    // already isolates transactions at the connection level — two
    // threads cannot share a connection, and Qt's transaction() is
    // per-connection by definition. The mutex's stated DB-004 purpose
    // ("prevent interleaving") was a vestige of an earlier shared-
    // connection model; with the per-thread registry it bought nothing
    // at the SQL layer and cost (a) global serialization of zone/rule/
    // reid saves that have no SQL conflict, and (b) permanent deadlock
    // if any begin/commit path threw before unlocking. Audit + risk
    // analysis logged in past-bugs.md.

    // Mutex protecting connection name registry (for close/cleanup)
    std::mutex connection_registry_mutex_;
    std::vector<QString> registered_connections_;

    // ── Event Batch Writer ──────────────────────────────────────────────
    void startBatchWriter();
    void stopBatchWriter();
    void batchWriterLoop();
    bool flushEventBatch(std::deque<Event>& batch);

    std::thread batch_writer_thread_;
    // mutable so batchWriterStats() can take it via try_to_lock without
    // affecting the const-correctness of the accessor.
    mutable std::mutex batch_queue_mutex_;
    std::condition_variable batch_cv_;
    std::deque<Event> event_queue_;
    std::atomic<bool> batch_writer_running_{false};
    std::atomic<bool> accepting_events_{false};

    // 2026-05-15 DB hot-path audit: batch writer observability counters.
    // Producer increments enqueued_total / dropped_total under batch_queue_mutex_;
    // batch writer thread updates flushed_total / flush_failures_total / row_failures_total.
    // peak_queue_depth uses a CAS retry loop in enqueueEvent.
    std::atomic<std::uint64_t> enqueued_total_{0};
    std::atomic<std::uint64_t> dropped_total_{0};
    std::atomic<std::uint64_t> flushed_total_{0};
    std::atomic<std::uint64_t> flush_failures_total_{0};
    std::atomic<std::uint64_t> row_failures_total_{0};
    std::atomic<std::uint64_t> peak_queue_depth_{0};

    static constexpr size_t kBatchFlushSize = 50;
    static constexpr int kBatchFlushIntervalMs = 100;
    static constexpr size_t kMaxQueueSize = 10000;
};

// ── RAII Transaction Guard ─────────────────────────────────────────────────
// Auto-rollback if begin succeeded but commit() was never called (e.g. an
// exception thrown between beginTransaction() and commit()). Each thread
// has its own QSqlDatabase, so rollback is local to the caller's connection.
// Usage:
//   {
//       TransactionGuard txn(DbManager::getInstance());
//       // ... execute queries ...
//       txn.commit();  // explicit commit
//   }  // auto-rollback if commit() was never called
//
class TransactionGuard {
public:
    explicit TransactionGuard(DbManager& db) : db_(db) {
        db_.beginTransaction();
    }

    ~TransactionGuard() {
        if (!finished_) {
            try { db_.rollback(); } catch (...) {}
        }
    }

    void commit()   { db_.commit();   finished_ = true; }
    void rollback() { db_.rollback(); finished_ = true; }

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

private:
    DbManager& db_;
    bool finished_{false};
};

} // namespace database
} // namespace vms
