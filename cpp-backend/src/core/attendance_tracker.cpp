// ==============================================================
// File: src/core/attendance_tracker.cpp
// ==============================================================

#include "core/attendance_tracker.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "database/db_manager.h"
#include "utils/logger.h"

namespace vms::core {

namespace {

int readCooldownSecondsFromEnv() {
    const char* e = std::getenv("VMS_ATTENDANCE_DEDUP_SECONDS");
    if (!e || !*e) return 60;
    try {
        int v = std::atoi(e);
        if (v < 1) v = 1;
        if (v > 24 * 3600) v = 24 * 3600;
        return v;
    } catch (...) {
        return 60;
    }
}

// Bulk insert one batch. Wraps in a transaction when supported; falls back to
// per-row autocommit if the driver returns false to begin().
bool flushAttendance(QSqlDatabase& db, std::deque<AttendanceRow>& batch) {
    if (batch.empty()) return true;
    const bool in_tx = db.transaction();
    if (!in_tx) {
        LOG_THROTTLED_WARN(10000,
            "AttendanceTracker: BEGIN failed (driver may not support tx): {}",
            db.lastError().text().toStdString());
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO attendance_events "
        "(person_id, employee_id, camera_id, kind, timestamp, confidence, "
        " snapshot_path, source_rule, metadata_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));

    int written = 0;
    for (const auto& r : batch) {
        q.bindValue(0, r.person_id);
        q.bindValue(1, r.employee_id > 0 ? QVariant(r.employee_id) : QVariant(QVariant::Int));
        q.bindValue(2, r.camera_id);
        q.bindValue(3, QString::fromStdString(r.kind));
        q.bindValue(4, static_cast<qlonglong>(r.timestamp_s));
        q.bindValue(5, r.confidence);
        q.bindValue(6, QString::fromStdString(r.snapshot_path));
        q.bindValue(7, QString::fromStdString(r.source_rule));
        q.bindValue(8, QString::fromStdString(r.metadata_json));
        if (!q.exec()) {
            LOG_WARN("AttendanceTracker: row insert failed: {}",
                     q.lastError().text().toStdString());
            continue;  // drop the row, don't abort the batch
        }
        ++written;
    }

    if (in_tx) {
        if (!db.commit()) {
            LOG_WARN("AttendanceTracker: COMMIT failed: {}", db.lastError().text().toStdString());
            db.rollback();
            return false;
        }
    }

    LOG_THROTTLED_INFO(60000,
        "AttendanceTracker: flushed {} attendance rows", written);
    return true;
}

}  // namespace

AttendanceTracker& AttendanceTracker::getInstance() {
    static AttendanceTracker inst;
    return inst;
}

AttendanceTracker::AttendanceTracker()
    : cooldown_seconds_(readCooldownSecondsFromEnv()),
      writer_(std::make_unique<vms::utils::BulkWriter<AttendanceRow>>(
          "attendance_writer", &flushAttendance)) {}

AttendanceTracker::~AttendanceTracker() { stop(); }

void AttendanceTracker::start() {
    if (started_.exchange(true)) return;
    reloadCameraRoles();
    reloadEmployees();
    writer_->start();
    LOG_INFO("AttendanceTracker: started (cooldown={}s, persons_cached={}, cameras_cached={})",
             cooldown_seconds_, emp_by_person_.size(), camera_role_.size());
}

void AttendanceTracker::stop() {
    if (!started_.exchange(false)) return;
    if (writer_) writer_->stop();
    LOG_INFO("AttendanceTracker: stopped");
}

std::string AttendanceTracker::resolveKind(int camera_id, uint64_t key) {
    std::string role;
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        auto it = camera_role_.find(camera_id);
        if (it != camera_role_.end()) role = it->second;
    }
    std::string last_kind;
    if (role == "both") {
        std::lock_guard<std::mutex> lk(dedup_mu_);
        auto it = dedup_.find(key);
        if (it != dedup_.end()) last_kind = it->second.last_kind;
        // last_kind defaults to empty → kindForRole returns "in" for the
        // first punch (since "in" != "in" is false → toggle to "out" only
        // when the previous was "in").
    }
    return kindForRole(role, last_kind);
}

int AttendanceTracker::resolveEmployeeId(int person_id) {
    std::lock_guard<std::mutex> lk(cache_mu_);
    auto it = emp_by_person_.find(person_id);
    return it == emp_by_person_.end() ? -1 : it->second;
}

void AttendanceTracker::onFaceRecognized(int camera_id,
                                        int person_id,
                                        float confidence,
                                        int64_t ts_seconds,
                                        const std::string& snapshot_path) {
    if (!started_.load(std::memory_order_acquire)) return;
    if (person_id <= 0) return;  // unknown faces never count

    const uint64_t key = packKey(person_id, camera_id);

    // Dedup window check via pure helper — keeps prod/test in sync.
    {
        std::lock_guard<std::mutex> lk(dedup_mu_);
        auto it = dedup_.find(key);
        if (it != dedup_.end() &&
            !isOutsideCooldownWindow(it->second.last_ts_s, ts_seconds, cooldown_seconds_)) {
            return;  // within cooldown — drop silently
        }
    }

    const std::string kind = resolveKind(camera_id, key);

    AttendanceRow row;
    row.person_id     = person_id;
    row.employee_id   = resolveEmployeeId(person_id);
    row.camera_id     = camera_id;
    row.kind          = kind;
    row.timestamp_s   = ts_seconds;
    row.confidence    = confidence;
    row.snapshot_path = snapshot_path;
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        auto it = camera_role_.find(camera_id);
        row.source_rule = (it != camera_role_.end()) ? "door_role" : "min_max_fallback";
    }

    // Update dedup BEFORE enqueue so concurrent calls see the new state.
    {
        std::lock_guard<std::mutex> lk(dedup_mu_);
        dedup_[key] = {ts_seconds, kind};
        // Opportunistic GC: shrink if >50k entries.
        if (dedup_.size() > 50000) {
            for (auto it = dedup_.begin(); it != dedup_.end(); ) {
                if (ts_seconds - it->second.last_ts_s > 24 * 3600) it = dedup_.erase(it);
                else ++it;
            }
        }
    }

    writer_->enqueue(std::move(row));
}

bool AttendanceTracker::recordManual(int employee_id,
                                    int camera_id,
                                    const std::string& kind,
                                    int64_t ts_seconds,
                                    std::string* err) {
    if (employee_id <= 0) {
        if (err) *err = "employee_id required";
        return false;
    }
    if (kind != "in" && kind != "out" && kind != "seen") {
        if (err) *err = "kind must be in|out|seen";
        return false;
    }

    // Resolve person_id back from employee_id (rare path — direct query).
    int person_id = -1;
    try {
        QSqlDatabase db = vms::database::DbManager::getInstance().getThreadConnection();
        QSqlQuery q(db);
        q.prepare("SELECT person_id FROM employees WHERE id = ?");
        q.bindValue(0, employee_id);
        if (q.exec() && q.next()) person_id = q.value(0).toInt();
    } catch (...) {}

    AttendanceRow row;
    row.person_id     = person_id;
    row.employee_id   = employee_id;
    row.camera_id     = camera_id;
    row.kind          = kind;
    row.timestamp_s   = ts_seconds;
    row.confidence    = 1.0f;
    row.source_rule   = "manual";

    writer_->enqueue(std::move(row));
    return true;
}

void AttendanceTracker::reloadEmployees() {
    std::unordered_map<int, int> next;
    try {
        QSqlDatabase db = vms::database::DbManager::getInstance().getThreadConnection();
        QSqlQuery q(db);
        if (q.exec("SELECT id, person_id FROM employees WHERE active = 1")) {
            while (q.next()) {
                next[q.value(1).toInt()] = q.value(0).toInt();
            }
        } else {
            LOG_WARN("AttendanceTracker: reloadEmployees query failed: {}",
                     q.lastError().text().toStdString());
        }
    } catch (const std::exception& e) {
        LOG_WARN("AttendanceTracker: reloadEmployees exception: {}", e.what());
    }

    std::lock_guard<std::mutex> lk(cache_mu_);
    emp_by_person_.swap(next);
}

void AttendanceTracker::reloadCameraRoles() {
    std::unordered_map<int, std::string> next;
    try {
        QSqlDatabase db = vms::database::DbManager::getInstance().getThreadConnection();
        QSqlQuery q(db);
        if (q.exec("SELECT camera_id, role FROM camera_roles")) {
            while (q.next()) {
                next[q.value(0).toInt()] = q.value(1).toString().toStdString();
            }
        } else {
            LOG_WARN("AttendanceTracker: reloadCameraRoles query failed: {}",
                     q.lastError().text().toStdString());
        }
    } catch (const std::exception& e) {
        LOG_WARN("AttendanceTracker: reloadCameraRoles exception: {}", e.what());
    }

    std::lock_guard<std::mutex> lk(cache_mu_);
    camera_role_.swap(next);
}

std::size_t AttendanceTracker::pendingRows() {
    return writer_ ? writer_->queueSize() : 0;
}

std::uint64_t AttendanceTracker::droppedRows() {
    return writer_ ? writer_->droppedCount() : 0;
}

}  // namespace vms::core
