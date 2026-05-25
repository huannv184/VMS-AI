// ==============================================================
// File: tests/integration/integration_test_db.h
// Fixture base class for DB-backed integration tests.
//
// Spins up a real `DbManager` against a per-test temp SQLite file
// (NOT `:memory:` — each Qt connection to `:memory:` is its own
// isolated DB, so the primary-vs-thread-connection split breaks).
// Initializes the full production schema via `DbManager::init` so
// any column/migration change in db_manager.cpp is automatically
// reflected here — tests can't drift from prod schema.
//
// Per-test lifecycle:
//   SetUp    → create temp .db file, init DbManager pointing at it
//   TearDown → DbManager::close() (joins batch writer thread),
//              remove the temp .db file (+ -wal/-shm sidecars)
//
// PR-7 pilot (2026-05-25): first DB-backed integration test in
// the project. If/when PR-7B + PR-7C land, factor any shared
// helpers (event insertion, line config insertion) out of the
// per-module test file into this header.
//
// IMPORTANT for test authors using this fixture:
//   1. Test main() MUST create a `QCoreApplication app(argc, argv)`
//      before InitGoogleTest. Without it, the Qt plugin loader
//      doesn't bootstrap and QSqlDatabase::open() SEH-crashes
//      dereferencing a NULL driver pointer (no error, no log —
//      just access violation).
//   2. Test exe MUST be invoked via `ctest`, NOT directly. The
//      CMakeLists sets QT_PLUGIN_PATH + PATH per test via
//      `set_tests_properties(... ENVIRONMENT ...)`. Direct
//      invocation skips that env setup and the SQLite driver
//      plugin (qsqlited.dll in Debug, qsqlite.dll in Release)
//      isn't discoverable.
// ==============================================================

#pragma once

#include <gtest/gtest.h>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>

#include "database/db_manager.h"
#include "utils/config.h"

namespace vms::test {

class IntegrationTestDb : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique-per-test temp DB file. Pure rng (no pid) — _getpid pulls in
        // <process.h> on Windows and the marginal collision safety isn't
        // worth the platform-specific include.
        thread_local std::mt19937_64 rng{std::random_device{}()};
        std::ostringstream oss;
        oss << "vms_itest_" << rng() << ".db";
        try {
            db_path_ = (std::filesystem::temp_directory_path() / oss.str()).string();
        } catch (const std::exception& e) {
            FAIL() << "temp_directory_path() threw: " << e.what();
        }

        vms::Config::DatabaseConfig cfg;
        cfg.driver = "sqlite";
        cfg.sqlite.path = db_path_;
        cfg.sqlite.busy_timeout_ms = 5000;

        bool ok = false;
        try {
            ok = vms::database::DbManager::getInstance().init(cfg);
        } catch (const std::exception& e) {
            FAIL() << "DbManager::init threw for " << db_path_ << ": " << e.what();
        }
        ASSERT_TRUE(ok) << "DbManager::init returned false for " << db_path_;
    }

    void TearDown() override {
        try {
            vms::database::DbManager::getInstance().close();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "DbManager::close threw: " << e.what();
        }
        // Remove main file and SQLite WAL sidecars. Ignore errors — temp
        // dir cleans up eventually even if a handle is briefly held.
        for (const auto& suffix : {"", "-wal", "-shm", "-journal"}) {
            std::error_code ec;
            std::filesystem::remove(db_path_ + suffix, ec);
        }
    }

    // Direct SQL helper — bypasses the batch writer and EventManager so
    // tests don't have to wait on the async flush path. Tests using this
    // are exercising the aggregator's read path against real SQLite, not
    // the insertion path (covered by other tests / by production usage).
    void insertEvent(int camera_id,
                     const std::string& event_type,
                     int64_t timestamp_s,
                     const std::string& metadata_json,
                     const std::string& event_id = "") {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        ASSERT_TRUE(db.isValid() && db.isOpen());

        QSqlQuery q(db);
        q.prepare("INSERT INTO events (id, camera_id, event_type, timestamp, metadata_json) "
                  "VALUES (?, ?, ?, ?, ?)");
        // Deterministic id if caller didn't pass one — avoids INSERT OR
        // IGNORE silent drops on duplicate empty-string ids (the BUG-DB-01
        // pattern from `past-bugs.md`).
        const std::string id = event_id.empty()
            ? makeEventId(camera_id, timestamp_s, event_type)
            : event_id;
        q.bindValue(0, QString::fromStdString(id));
        q.bindValue(1, camera_id);
        q.bindValue(2, QString::fromStdString(event_type));
        q.bindValue(3, static_cast<qlonglong>(timestamp_s));
        q.bindValue(4, QString::fromStdString(metadata_json));
        ASSERT_TRUE(q.exec()) << "insertEvent failed: "
                              << q.lastError().text().toStdString();
    }

    void insertCountingLine(int line_id,
                            int camera_id,
                            const std::string& name = "test-line",
                            float ax = 0.5f, float ay = 0.0f,
                            float bx = 0.5f, float by = 1.0f) {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        ASSERT_TRUE(db.isValid() && db.isOpen());

        QSqlQuery q(db);
        q.prepare("INSERT INTO counting_lines "
                  "(id, camera_id, name, ax, ay, bx, by, "
                  " direction_a_label, direction_b_label, "
                  " object_classes_json, enabled) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, 'in', 'out', '[\"person\"]', 1)");
        q.bindValue(0, line_id);
        q.bindValue(1, camera_id);
        q.bindValue(2, QString::fromStdString(name));
        q.bindValue(3, ax);
        q.bindValue(4, ay);
        q.bindValue(5, bx);
        q.bindValue(6, by);
        ASSERT_TRUE(q.exec()) << "insertCountingLine failed: "
                              << q.lastError().text().toStdString();
    }

    // Count rows in counter_buckets_1m matching the filter — used by tests
    // to assert the aggregator UPSERTed the expected number of buckets.
    int countBuckets(int camera_id = -1, int source_id = -1) {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        std::string sql = "SELECT COUNT(*) FROM counter_buckets_1m WHERE 1=1";
        if (camera_id >= 0) sql += " AND camera_id = " + std::to_string(camera_id);
        if (source_id >= 0) sql += " AND source_id = " + std::to_string(source_id);
        QSqlQuery q(db);
        EXPECT_TRUE(q.exec(QString::fromStdString(sql)));
        return q.next() ? q.value(0).toInt() : -1;
    }

    // Sum aggregation — mirrors the GET /api/counter/summary query in
    // counter_controller.cpp. Caller asserts the returned (in, out) pair
    // matches expectations, which proves both the aggregator's write
    // AND the summary read against real SQLite.
    std::pair<int, int> summarySumForCamera(int camera_id,
                                            int64_t from_ts,
                                            int64_t to_ts) {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        QSqlQuery q(db);
        q.prepare("SELECT COALESCE(SUM(in_count), 0), COALESCE(SUM(out_count), 0) "
                  "FROM counter_buckets_1m "
                  "WHERE camera_id = ? AND source_kind = 'line' "
                  "  AND ts_minute >= ? AND ts_minute < ?");
        q.bindValue(0, camera_id);
        q.bindValue(1, static_cast<qlonglong>(from_ts));
        q.bindValue(2, static_cast<qlonglong>(to_ts));
        EXPECT_TRUE(q.exec());
        if (q.next()) return {q.value(0).toInt(), q.value(1).toInt()};
        return {-1, -1};
    }

    // A timestamp safely inside the aggregator's [now - 5min, now - 1min)
    // window. Use this for events that the test wants to be picked up by
    // sweepOnce. `offset_s` lets the test stagger events within the window
    // (e.g. force two into the same minute or into different minutes).
    static int64_t wellInsideWindow(int offset_s = 0) {
        const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // Pick (now - 120s) as the centre. Aggregator window is
        // [now_min - 300s, now_min - 60s) so 120s ago is comfortably inside.
        return (now_s - 120) + offset_s;
    }

private:
    static std::string makeEventId(int camera_id, int64_t ts, const std::string& etype) {
        std::ostringstream oss;
        oss << "test_" << camera_id << "_" << ts << "_" << etype << "_"
            << (rand() % 1000000);
        return oss.str();
    }

    std::string db_path_;
};

} // namespace vms::test
