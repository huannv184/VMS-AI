#include "database/segment_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"

#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>

namespace vms {
namespace database {

bool SegmentRepository::insertSegment(int camera_id, const std::string& filename,
                                       time_t start_time, time_t end_time, size_t file_size,
                                       const std::string& status) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(
        "INSERT INTO recording_segments (camera_id, filename, start_time, end_time, file_size, status) "
        "VALUES (?, ?, ?, ?, ?, ?)"
    );

    query.bindValue(0, camera_id);
    query.bindValue(1, QString::fromStdString(filename));
    query.bindValue(2, static_cast<qlonglong>(start_time));
    query.bindValue(3, static_cast<qlonglong>(end_time));
    query.bindValue(4, static_cast<qlonglong>(file_size));
    query.bindValue(5, QString::fromStdString(status));

    if (!query.exec()) {
        LOG_ERROR("SegmentRepository::insertSegment failed: {}", query.lastError().text().toStdString());
        return false;
    }

    LOG_INFO("[SegmentRepo] Inserted segment: cam={}, file={}, {}s", camera_id, filename, end_time - start_time);
    return true;
}

std::vector<RecordingSegment> SegmentRepository::getSegments(int camera_id,
                                                              time_t from_ts,
                                                              time_t to_ts) {
    std::vector<RecordingSegment> results;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return results;

    std::string sql =
        "SELECT id, camera_id, filename, start_time, end_time, file_size, status, created_at "
        "FROM recording_segments "
        "WHERE camera_id = ?";

    if (from_ts > 0) sql += " AND end_time >= " + std::to_string(from_ts);
    if (to_ts > 0)   sql += " AND start_time <= " + std::to_string(to_ts);
    sql += " ORDER BY start_time ASC";

    QSqlQuery query(db);
    query.prepare(QString::fromStdString(sql));
    query.bindValue(0, camera_id);

    if (!query.exec()) {
        LOG_ERROR("SegmentRepository::getSegments failed: {}", query.lastError().text().toStdString());
        return results;
    }

    while (query.next()) {
        RecordingSegment seg;
        seg.id = query.value(0).toInt();
        seg.camera_id = query.value(1).toInt();
        seg.filename = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        seg.start_time = static_cast<time_t>(query.value(3).toLongLong());
        seg.end_time = static_cast<time_t>(query.value(4).toLongLong());
        seg.file_size = static_cast<size_t>(query.value(5).toLongLong());
        seg.status = query.value(6).isNull() ? "completed" : query.value(6).toString().toStdString();
        seg.created_at = static_cast<time_t>(query.value(7).toLongLong());
        results.push_back(seg);
    }

    return results;
}

RecordingSegment SegmentRepository::getSegmentById(int segment_id) {
    RecordingSegment seg;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return seg;

    QSqlQuery query(db);
    query.prepare(
        "SELECT id, camera_id, filename, start_time, end_time, file_size, status, created_at "
        "FROM recording_segments WHERE id = ?"
    );
    query.bindValue(0, segment_id);

    if (!query.exec()) {
        return seg;
    }

    if (query.next()) {
        seg.id = query.value(0).toInt();
        seg.camera_id = query.value(1).toInt();
        seg.filename = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        seg.start_time = static_cast<time_t>(query.value(3).toLongLong());
        seg.end_time = static_cast<time_t>(query.value(4).toLongLong());
        seg.file_size = static_cast<size_t>(query.value(5).toLongLong());
        seg.status = query.value(6).isNull() ? "completed" : query.value(6).toString().toStdString();
        seg.created_at = static_cast<time_t>(query.value(7).toLongLong());
    }

    return seg;
}

int SegmentRepository::getSegmentCount(int camera_id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    query.prepare("SELECT COUNT(*) FROM recording_segments WHERE camera_id = ?");
    query.bindValue(0, camera_id);

    if (!query.exec()) {
        return 0;
    }

    if (query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

bool SegmentRepository::deleteOldSegments(time_t before_ts) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare("DELETE FROM recording_segments WHERE end_time < ?");
    query.bindValue(0, static_cast<qlonglong>(before_ts));

    if (!query.exec()) {
        LOG_ERROR("SegmentRepository::deleteOldSegments failed: {}", query.lastError().text().toStdString());
        return false;
    }

    int deleted = query.numRowsAffected();
    if (deleted > 0) {
        LOG_INFO("[SegmentRepo] Pruned {} old segments", deleted);
    }
    return true;
}

bool SegmentRepository::updateSegment(int segment_id, const std::string& status, size_t file_size) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare("UPDATE recording_segments SET status = ?, file_size = ? WHERE id = ?");
    query.bindValue(0, QString::fromStdString(status));
    query.bindValue(1, static_cast<qlonglong>(file_size));
    query.bindValue(2, segment_id);

    return query.exec();
}

} // namespace database
} // namespace vms
