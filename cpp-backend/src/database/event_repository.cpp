#include "database/event_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"

#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace vms {
namespace database {

namespace {

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void bindEventQueryResult(QSqlQuery& query, std::vector<Event>& events) {
    while (query.next()) {
        Event event;
        event.id = query.value(0).isNull() ? "" : query.value(0).toString().toStdString();
        event.camera_id = query.value(1).toInt();
        event.event_type = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        event.description = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        event.snapshot_path = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        event.metadata_json = query.value(5).isNull() ? "{}" : query.value(5).toString().toStdString();
        event.timestamp = query.value(6).toLongLong();
        event.video_path = query.value(7).isNull() ? "" : query.value(7).toString().toStdString();
        event.duration = query.value(8).toInt();
        event.severity = query.value(9).isNull() ? "low" : query.value(9).toString().toStdString();
        events.push_back(event);
    }
}

} // namespace

std::vector<Event> EventRepository::getEvents(int camera_id,
                                              int limit,
                                              int offset,
                                              const std::string& event_type,
                                              std::optional<long long> start_time,
                                              std::optional<long long> end_time) {
    std::vector<Event> events;

    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return events;
    }

    std::string sql = "SELECT id, camera_id, event_type, description, snapshot_path, metadata_json, timestamp, video_path, duration, severity FROM events";
    std::vector<std::string> where_clauses;

    if (camera_id >= 0) {
        where_clauses.emplace_back("camera_id = ?");
    }

    const std::string normalized_type = toLowerCopy(event_type);
    if (!normalized_type.empty()) {
        // BUG-22: ZMQ handlers normalised event_type to UPPERCASE on 2026-04-27.
        // Bind both cases so lowercase api filter still finds new uppercase rows.
        if (normalized_type == "ppe" || normalized_type == "ppe_violation") {
            where_clauses.emplace_back("(event_type = ? OR event_type = ? OR event_type = ?)");
        } else if (normalized_type == "face" || normalized_type == "face_recognition" || normalized_type == "face_recognized") {
            where_clauses.emplace_back("(event_type = ? OR event_type = ? OR event_type = ?)");
        } else if (normalized_type == "detection") {
            where_clauses.emplace_back("(event_type = ? OR event_type LIKE ? OR event_type LIKE ?)");
        } else if (normalized_type == "intrusion" || normalized_type == "loitering"
                || normalized_type == "fire" || normalized_type == "smoke") {
            where_clauses.emplace_back("(event_type = ? OR event_type = ?)");
        } else {
            where_clauses.emplace_back("event_type = ?");
        }
    }

    if (start_time.has_value()) {
        where_clauses.emplace_back("timestamp >= ?");
    }
    if (end_time.has_value()) {
        where_clauses.emplace_back("timestamp <= ?");
    }

    if (!where_clauses.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < where_clauses.size(); ++i) {
            if (i > 0) sql += " AND ";
            sql += where_clauses[i];
        }
    }

    sql += " ORDER BY timestamp DESC LIMIT ? OFFSET ?";

    QSqlQuery query(db);
    query.prepare(QString::fromStdString(sql));

    int bind_idx = 0;
    if (camera_id >= 0) {
        query.bindValue(bind_idx++, camera_id);
    }

    if (!normalized_type.empty()) {
        if (normalized_type == "ppe" || normalized_type == "ppe_violation") {
            query.bindValue(bind_idx++, QStringLiteral("ppe"));
            query.bindValue(bind_idx++, QStringLiteral("ppe_violation"));
            query.bindValue(bind_idx++, QStringLiteral("PPE_VIOLATION"));
        } else if (normalized_type == "face" || normalized_type == "face_recognition" || normalized_type == "face_recognized") {
            query.bindValue(bind_idx++, QStringLiteral("face"));
            query.bindValue(bind_idx++, QStringLiteral("face_recognition"));
            query.bindValue(bind_idx++, QStringLiteral("FACE_RECOGNIZED"));
        } else if (normalized_type == "detection") {
            query.bindValue(bind_idx++, QStringLiteral("detection"));
            query.bindValue(bind_idx++, QStringLiteral("%_detected"));
            query.bindValue(bind_idx++, QStringLiteral("%_DETECTED"));
        } else if (normalized_type == "intrusion") {
            query.bindValue(bind_idx++, QStringLiteral("intrusion"));
            query.bindValue(bind_idx++, QStringLiteral("INTRUSION"));
        } else if (normalized_type == "loitering") {
            query.bindValue(bind_idx++, QStringLiteral("loitering"));
            query.bindValue(bind_idx++, QStringLiteral("LOITERING"));
        } else if (normalized_type == "fire") {
            query.bindValue(bind_idx++, QStringLiteral("fire"));
            query.bindValue(bind_idx++, QStringLiteral("FIRE"));
        } else if (normalized_type == "smoke") {
            query.bindValue(bind_idx++, QStringLiteral("smoke"));
            query.bindValue(bind_idx++, QStringLiteral("SMOKE"));
        } else {
            query.bindValue(bind_idx++, QString::fromStdString(event_type));
        }
    }
    if (start_time.has_value()) {
        query.bindValue(bind_idx++, static_cast<qlonglong>(start_time.value()));
    }
    if (end_time.has_value()) {
        query.bindValue(bind_idx++, static_cast<qlonglong>(end_time.value()));
    }
    query.bindValue(bind_idx++, limit);
    query.bindValue(bind_idx++, offset);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute query: {}", query.lastError().text().toStdString());
        return events;
    }

    bindEventQueryResult(query, events);

    return events;
}

std::optional<Event> EventRepository::getEventById(const std::string& id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return std::nullopt;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, camera_id, event_type, description, snapshot_path, metadata_json, timestamp, video_path, duration, severity FROM events WHERE id = ?");
    query.bindValue(0, QString::fromStdString(id));

    if (!query.exec()) {
        return std::nullopt;
    }

    if (query.next()) {
        Event event;
        event.id = query.value(0).isNull() ? "" : query.value(0).toString().toStdString();
        event.camera_id = query.value(1).toInt();
        event.event_type = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        event.description = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        event.snapshot_path = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        event.metadata_json = query.value(5).isNull() ? "{}" : query.value(5).toString().toStdString();
        event.timestamp = query.value(6).toLongLong();
        event.video_path = query.value(7).isNull() ? "" : query.value(7).toString().toStdString();
        event.duration = query.value(8).toInt();
        event.severity = query.value(9).isNull() ? "low" : query.value(9).toString().toStdString();

        return event;
    }

    return std::nullopt;
}

bool EventRepository::insertEvent(const Event& event) {
    // B-4 FIX: Delegate to batch writer for async insertion
    return DbManager::getInstance().enqueueEvent(event);
}

bool EventRepository::deleteEvent(const std::string& id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return false;
    }

    QSqlQuery query(db);
    query.prepare("DELETE FROM events WHERE id = ?");
    query.bindValue(0, QString::fromStdString(id));

    if (!query.exec()) {
        return false;
    }

    return true;
}

int EventRepository::getEventCount(int camera_id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return 0;
    }

    std::string sql = "SELECT COUNT(*) FROM events";
    if (camera_id >= 0) {
        sql += " WHERE camera_id = " + std::to_string(camera_id);
    }

    QSqlQuery query(db);
    query.prepare(QString::fromStdString(sql));

    if (!query.exec()) {
        return 0;
    }

    if (query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

bool EventRepository::updateEventVideo(const std::string& id, const std::string& video_path, int duration_sec) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return false;
    }

    QSqlQuery query(db);
    query.prepare("UPDATE events SET video_path = ?, duration = ? WHERE id = ?");
    query.bindValue(0, QString::fromStdString(video_path));
    query.bindValue(1, duration_sec);
    query.bindValue(2, QString::fromStdString(id));

    if (!query.exec()) {
        LOG_ERROR("Failed to update event video: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

std::vector<Event> EventRepository::getRecordingEvents(int camera_id, int limit, int offset) {
    std::vector<Event> events;

    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return events;
    }

    // Only select events that have a non-empty video_path
    std::string sql = "SELECT id, camera_id, event_type, description, snapshot_path, metadata_json, timestamp, video_path, duration, severity FROM events WHERE video_path IS NOT NULL AND video_path != ''";

    if (camera_id >= 0) {
        sql += " AND camera_id = ?";
    }

    sql += " ORDER BY timestamp DESC LIMIT ? OFFSET ?";

    QSqlQuery query(db);
    query.prepare(QString::fromStdString(sql));

    int bind_idx = 0;
    if (camera_id >= 0) {
        query.bindValue(bind_idx++, camera_id);
    }
    query.bindValue(bind_idx++, limit);
    query.bindValue(bind_idx++, offset);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute recording query: {}", query.lastError().text().toStdString());
        return events;
    }

    bindEventQueryResult(query, events);

    return events;
}

int EventRepository::getRecordingCount(int camera_id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return 0;
    }

    std::string sql = "SELECT COUNT(*) FROM events WHERE video_path IS NOT NULL AND video_path != ''";
    if (camera_id >= 0) {
        sql += " AND camera_id = " + std::to_string(camera_id);
    }

    QSqlQuery query(db);
    query.prepare(QString::fromStdString(sql));

    if (!query.exec()) {
        return 0;
    }

    if (query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

bool EventRepository::pruneOld(time_t threshold) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return false;

    // Retrieve snapshot_paths of old events to delete from disk
    QSqlQuery selQuery(db);
    selQuery.prepare("SELECT snapshot_path FROM events WHERE timestamp < ? AND snapshot_path IS NOT NULL AND snapshot_path != ''");
    selQuery.bindValue(0, static_cast<qlonglong>(threshold));
    
    std::vector<std::string> files_to_delete;
    if (selQuery.exec()) {
        while (selQuery.next()) {
            files_to_delete.push_back(selQuery.value(0).toString().toStdString());
        }
    }

    QSqlQuery query(db);
    query.prepare("DELETE FROM events WHERE timestamp < ?");
    query.bindValue(0, static_cast<qlonglong>(threshold));

    if (!query.exec()) {
        LOG_ERROR("EventRepository::pruneOld failed: {}", query.lastError().text().toStdString());
        return false;
    }

    int affected = query.numRowsAffected();
    if (affected > 0) {
        LOG_INFO("[EventRepo] Pruned {} old events from DB", affected);
        
        // Physically delete snapshot files
        int deleted_files = 0;
        for (const auto& path : files_to_delete) {
            if (std::remove(path.c_str()) == 0) {
                deleted_files++;
            }
        }
        LOG_INFO("[EventRepo] Deleted {} old snapshot files from disk", deleted_files);
    }
    return true;
}

} // namespace database
} // namespace vms
