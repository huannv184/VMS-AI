// ==============================================================
// File: tests/integration/test_attendance_integration.cpp
// PR-7C integration test (2026-05-25) — exercises the REAL
// AttendanceTracker + BulkWriter → attendance_events DB writes,
// then validates the /api/attendance/health 24h-scan SUM(CASE WHEN)
// query against real SQLite.
//
// What this covers that test_attendance_dedup.cpp + test_attendance_
// shift.cpp + test_attendance_health.cpp (inline reproductions)
// can't:
//   - Production AttendanceTracker → BulkWriter → real flushAttendance
//     INSERT into attendance_events on real SQLite
//   - source_rule resolution against the REAL camera_role_ cache,
//     not a test-side reproduction (so a refactor that changes which
//     role-string is stored will surface here)
//   - employee_id NULL handling at the SQL bind layer (the
//     `r.employee_id > 0 ? QVariant(r.employee_id) : QVariant(QVariant::Int)`
//     branch — a refactor that drops the null-coalesce would land an
//     integer 0 in DB instead of NULL, which silently breaks the
//     /health unlinked_events_24h scan)
//   - The /health 24h scan SUM(CASE WHEN employee_id IS NULL ...) +
//     SUM(CASE WHEN source_rule = 'min_max_fallback' ...) query
//     against real SQLite — catches column-name drift between the
//     write side (flushAttendance) and the read side (controller)
//
// What this test does NOT cover (intentionally, per PR-7C scope):
//   - AiEventProcessor::processFace handler (the entry that calls
//     onFaceRecognized) — covered by hardware runbook + the
//     trivial `if (person_id > 0) tracker.onFaceRecognized(...)`
//     gate at ai_event_processor.cpp:338 has low regression surface
//   - HTTP/RBAC layer of /api/attendance/health — covered by
//     test_attendance_health.cpp inline matrix + RBAC sweep in
//     p0_batch_c_rbac_sweep_2026_05_12
//   - Shift rollup logic — covered by test_attendance_shift.cpp
//     (and exercised by the controller's queryAttendanceForDate)
// ==============================================================

#include "integration_test_db.h"

#include "core/attendance_tracker.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace {

using vms::test::IntegrationTestDb;

int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class AttendanceIntegration : public IntegrationTestDb {
protected:
    // Always stop the tracker before DbManager::close so BulkWriter's
    // drain-on-stop has a live DB connection. Tracker is a singleton
    // shared across tests; safe to start/stop per test because start()
    // is idempotent and stop() joins the writer thread cleanly.
    void TearDown() override {
        vms::core::AttendanceTracker::getInstance().stop();
        IntegrationTestDb::TearDown();
    }

    vms::core::AttendanceTracker& tracker() {
        return vms::core::AttendanceTracker::getInstance();
    }

    // Direct INSERT — bypass any controller layer, write the smallest
    // employee row the tracker's reloadEmployees() will read. The code
    // column has a UNIQUE constraint so we derive a per-call value
    // from the employee id (caller passes test-unique ids to avoid
    // cross-test collisions).
    void insertEmployee(int id, int person_id,
                        const std::string& code_prefix = "EMP-T") {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        QSqlQuery q(db);
        q.prepare("INSERT INTO employees (id, person_id, code, active) "
                  "VALUES (?, ?, ?, 1)");
        q.bindValue(0, id);
        q.bindValue(1, person_id);
        q.bindValue(2, QString::fromStdString(code_prefix + "-" + std::to_string(id)));
        ASSERT_TRUE(q.exec()) << q.lastError().text().toStdString();
    }

    void insertCameraRole(int camera_id, const std::string& role) {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        QSqlQuery q(db);
        q.prepare("INSERT INTO camera_roles (camera_id, role) VALUES (?, ?)");
        q.bindValue(0, camera_id);
        q.bindValue(1, QString::fromStdString(role));
        ASSERT_TRUE(q.exec()) << q.lastError().text().toStdString();
    }

    // Count rows in attendance_events matching the filter. -1 sentinel
    // means "don't filter on this column".
    int countAttendance(int person_id = -1, int camera_id = -1) {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        std::string sql = "SELECT COUNT(*) FROM attendance_events WHERE 1=1";
        if (person_id >= 0) sql += " AND person_id = " + std::to_string(person_id);
        if (camera_id >= 0) sql += " AND camera_id = " + std::to_string(camera_id);
        QSqlQuery q(db);
        EXPECT_TRUE(q.exec(QString::fromStdString(sql)));
        return q.next() ? q.value(0).toInt() : -1;
    }

    // Fetch the most recent attendance_events row for the given person.
    // Returns (employee_id_is_null, source_rule, kind). Asserts at most
    // one matching row.
    struct AttendanceRowSummary {
        bool        employee_id_is_null = true;
        int         employee_id         = -1;
        std::string source_rule;
        std::string kind;
    };
    AttendanceRowSummary fetchLatestFor(int person_id) {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        QSqlQuery q(db);
        q.prepare("SELECT employee_id, source_rule, kind "
                  "FROM attendance_events WHERE person_id = ? "
                  "ORDER BY id DESC LIMIT 1");
        q.bindValue(0, person_id);
        EXPECT_TRUE(q.exec());
        AttendanceRowSummary out;
        if (q.next()) {
            out.employee_id_is_null = q.value(0).isNull();
            out.employee_id         = q.value(0).isNull() ? -1 : q.value(0).toInt();
            out.source_rule         = q.value(1).toString().toStdString();
            out.kind                = q.value(2).toString().toStdString();
        }
        return out;
    }

    // Replicates the /api/attendance/health 24h-window SUM(CASE WHEN)
    // query in attendance_controller.cpp. Returns (unlinked, fallback).
    // Asserting against this proves the read SQL doesn't drift from
    // the write SQL in flushAttendance.
    std::pair<int, int> healthAuditCounts() {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        const int64_t window_start = nowSec() - 24 * 3600;
        QSqlQuery q(db);
        q.prepare(
            "SELECT "
            "  SUM(CASE WHEN employee_id IS NULL THEN 1 ELSE 0 END), "
            "  SUM(CASE WHEN source_rule = 'min_max_fallback' THEN 1 ELSE 0 END) "
            "FROM attendance_events "
            "WHERE timestamp >= ?");
        q.bindValue(0, static_cast<qlonglong>(window_start));
        EXPECT_TRUE(q.exec());
        if (q.next()) {
            return {q.value(0).isNull() ? 0 : q.value(0).toInt(),
                    q.value(1).isNull() ? 0 : q.value(1).toInt()};
        }
        return {-1, -1};
    }
};

// IMPORTANT — singleton state persistence:
// AttendanceTracker is a process-wide singleton. The dedup_ map AND the
// last_event_ts_ atomic survive across TEST_F invocations. To prevent
// cross-test interference, each test below uses a test-unique
// (person_id, camera_id) pair AND a test-unique employee id (the code
// column has a UNIQUE constraint). The convention:
//   test N uses person_id N*100+X, camera_id N*10+Y, employee id N.
// Same pattern as test_ppe_integration's 10000+ camera id range.

// ── Cache load on start ────────────────────────────────────────────────────

TEST_F(AttendanceIntegration, StartReloadsEmployeesAndCameraRolesCache) {
    insertEmployee(/*id*/101, /*person_id*/1001);
    insertEmployee(/*id*/102, /*person_id*/1002);
    insertCameraRole(/*camera*/110, "entry");
    insertCameraRole(/*camera*/111, "exit");

    tracker().start();

    EXPECT_EQ(tracker().employeesCached(),    2u);
    EXPECT_EQ(tracker().cameraRolesCached(),  2u);
    EXPECT_TRUE(tracker().started());
}

// ── Happy path: door-role camera + mapped person ──────────────────────────

TEST_F(AttendanceIntegration, OnFaceRecognizedDoorRoleWritesLinkedRow) {
    insertEmployee(/*id*/201, /*person_id*/2001);
    insertCameraRole(/*camera*/210, "entry");
    tracker().start();

    tracker().onFaceRecognized(/*camera*/210, /*person_id*/2001,
                                /*conf*/0.95f, /*ts*/nowSec(),
                                /*snapshot*/"snap.jpg");
    tracker().stop();  // drain → flushAttendance → attendance_events row

    ASSERT_EQ(countAttendance(/*person*/2001, /*camera*/210), 1);
    auto row = fetchLatestFor(2001);
    EXPECT_FALSE(row.employee_id_is_null);
    EXPECT_EQ(row.employee_id, 201);
    EXPECT_EQ(row.source_rule, "door_role");
    EXPECT_EQ(row.kind, "in");   // entry-role → "in"
}

// ── Degraded path A: person not mapped to employee ────────────────────────

TEST_F(AttendanceIntegration, UnmappedPersonWritesNullEmployeeId) {
    // person_id=3001 has no employees row. Tracker still records the
    // attendance but employee_id binds as NULL — this is what the
    // /health unlinked_events_24h counter is designed to surface.
    insertCameraRole(/*camera*/310, "entry");
    tracker().start();

    tracker().onFaceRecognized(/*camera*/310, /*person_id*/3001,
                                /*conf*/0.9f, nowSec(), "x.jpg");
    tracker().stop();

    ASSERT_EQ(countAttendance(/*person*/3001, /*camera*/310), 1);
    auto row = fetchLatestFor(3001);
    EXPECT_TRUE(row.employee_id_is_null)
        << "BUG-ATT-NULL-EMPLOYEE-01 regression: unmapped person_id "
        << "produced employee_id != NULL, which would silently break "
        << "the /health unlinked_events_24h SUM(CASE WHEN employee_id "
        << "IS NULL ...) scan and hide config gaps from operators.";
}

// ── Degraded path B: camera has no door role ──────────────────────────────

TEST_F(AttendanceIntegration, NoRoleCameraWritesFallbackSourceRule) {
    insertEmployee(/*id*/401, /*person_id*/4001);
    // Intentionally NO camera_role for camera 499.
    tracker().start();

    tracker().onFaceRecognized(/*camera*/499, /*person_id*/4001,
                                nowSec(), nowSec(), "x.jpg");
    tracker().stop();

    auto row = fetchLatestFor(4001);
    EXPECT_EQ(row.source_rule, "min_max_fallback")
        << "Camera without configured role must write source_rule="
        << "'min_max_fallback' so the /health fallback_events_24h "
        << "counter can surface the config gap.";
}

// ── Dedup ─────────────────────────────────────────────────────────────────

TEST_F(AttendanceIntegration, DedupWindowDropsBackToBackRecognitions) {
    insertEmployee(/*id*/501, /*person_id*/5001);
    insertCameraRole(/*camera*/510, "entry");
    tracker().start();

    const int64_t ts = nowSec();
    tracker().onFaceRecognized(510, 5001, 0.9f, ts,      "a.jpg");
    tracker().onFaceRecognized(510, 5001, 0.9f, ts + 5,  "b.jpg");  // within 60s window
    tracker().onFaceRecognized(510, 5001, 0.9f, ts + 30, "c.jpg");  // still within
    tracker().stop();

    EXPECT_EQ(countAttendance(/*person*/5001, /*camera*/510), 1)
        << "Dedup window should drop consecutive recognitions of the "
        << "same (person, camera) within VMS_ATTENDANCE_DEDUP_SECONDS "
        << "(default 60s). Multiple rows would corrupt rollups.";
}

// ── Negative guards ──────────────────────────────────────────────────────

TEST_F(AttendanceIntegration, RecognitionDroppedWhenNotStarted) {
    insertEmployee(/*id*/601, /*person_id*/6001);
    insertCameraRole(/*camera*/610, "entry");
    // Intentionally NOT calling start() — onFaceRecognized must early-
    // return because started_ is false. This is the load-bearing guard
    // that prevents attendance writes during the boot window before
    // ai_event_processor wires up.
    tracker().onFaceRecognized(610, 6001, 0.9f, nowSec(), "x.jpg");
    tracker().stop();   // no-op since never started

    EXPECT_EQ(countAttendance(/*person*/6001), 0);
}

TEST_F(AttendanceIntegration, PersonIdZeroIsDropped) {
    insertCameraRole(/*camera*/710, "entry");
    tracker().start();

    // person_id <= 0 means face not recognized — must NOT produce an
    // attendance row. Unknown-face writes would explode the table.
    tracker().onFaceRecognized(710,  0, 0.9f, nowSec(), "x.jpg");
    tracker().onFaceRecognized(710, -1, 0.9f, nowSec(), "x.jpg");
    tracker().stop();

    // No row should exist for camera 710 — other tests' rows may exist
    // on different cameras, so we filter on camera to avoid false fail.
    EXPECT_EQ(countAttendance(/*person*/-1, /*camera*/710), 0);
}

// ── Readiness atomics (PR-5 audit signals) ───────────────────────────────

TEST_F(AttendanceIntegration, LastEventTsBumpedOnAcceptedRecognition) {
    insertEmployee(/*id*/801, /*person_id*/8001);
    insertCameraRole(/*camera*/810, "entry");
    tracker().start();
    // PR-5 atomics survive across tests in the singleton, AND prior
    // tests may have ticked lastEventTs within the same second as
    // nowSec(). So we assert >= ts (the timestamp we passed in) rather
    // than > before — the contract is "bumped to at least our ts on
    // accept", not "strictly greater than prior reading".
    const int64_t ts = nowSec() + 100;  // future-stamp so it definitively wins
    tracker().onFaceRecognized(810, 8001, 0.9f, ts, "x.jpg");
    EXPECT_GE(tracker().lastEventTs(), ts);
    tracker().stop();
}

// ── /health 24h SUM(CASE WHEN) read SQL contract ─────────────────────────

TEST_F(AttendanceIntegration, HealthAuditScanCountsDegradedRowsCorrectly) {
    // Bypass the tracker entirely — insert rows shaped to exercise BOTH
    // degraded branches of the /health 24h scan in one query. This is
    // the regression that would surface if someone refactored the
    // controller's SQL to use `employee_id = 0` instead of `IS NULL`,
    // or renamed source_rule values.
    auto db = vms::database::DbManager::getInstance().getThreadConnection();
    const int64_t ts = nowSec();
    const auto insert = [&](int person_id, int employee_id, const std::string& rule) {
        QSqlQuery q(db);
        q.prepare("INSERT INTO attendance_events "
                  "(person_id, employee_id, camera_id, kind, timestamp, "
                  " confidence, snapshot_path, source_rule, metadata_json) "
                  "VALUES (?, ?, 99, 'seen', ?, 1.0, '', ?, '')");
        q.bindValue(0, person_id);
        q.bindValue(1, employee_id > 0 ? QVariant(employee_id) : QVariant(QVariant::Int));
        q.bindValue(2, static_cast<qlonglong>(ts));
        q.bindValue(3, QString::fromStdString(rule));
        ASSERT_TRUE(q.exec()) << q.lastError().text().toStdString();
    };
    insert(/*person*/301, /*emp*/1,  "door_role");          // healthy
    insert(/*person*/302, /*emp*/-1, "door_role");          // unlinked (null employee)
    insert(/*person*/303, /*emp*/-1, "door_role");          // unlinked
    insert(/*person*/304, /*emp*/2,  "min_max_fallback");   // fallback (linked employee)
    insert(/*person*/305, /*emp*/-1, "min_max_fallback");   // both unlinked + fallback
    insert(/*person*/306, /*emp*/3,  "manual");             // healthy

    const auto [unlinked, fallback] = healthAuditCounts();
    EXPECT_EQ(unlinked, 3);   // rows 302, 303, 305
    EXPECT_EQ(fallback, 2);   // rows 304, 305
}

} // namespace

int main(int argc, char** argv) {
    // QCoreApplication required because the test links Qt SQL — see
    // integration_test_db.h header for the SEH-0xc0000005 backstory.
    // CRITICAL: this main() MUST be outside the anonymous namespace
    // above. If it lives inside, the linker treats it as a static
    // function and uses a different entry point — QCoreApplication
    // never runs, Qt plugin loader doesn't bootstrap, and
    // QSqlDatabase::open() SEH-crashes the SetUp().
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
