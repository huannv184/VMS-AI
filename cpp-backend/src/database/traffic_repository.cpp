#include "database/traffic_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"

#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>

#include <ctime>
#include <string>

namespace vms {
namespace database {

bool TrafficRepository::insertCount(const TrafficCount& tc) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(
        "INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
    );

    query.bindValue(0, tc.camera_id);
    query.bindValue(1, tc.roi_id);
    query.bindValue(2, QString::fromStdString(tc.direction));
    query.bindValue(3, QString::fromStdString(tc.vehicle_type));
    query.bindValue(4, tc.count);
    query.bindValue(5, static_cast<qlonglong>(tc.period_start));
    query.bindValue(6, static_cast<qlonglong>(tc.period_end));
    query.bindValue(7, static_cast<qlonglong>(std::time(nullptr)));

    if (!query.exec()) {
        LOG_ERROR("TrafficRepository::insertCount failed: {}", query.lastError().text().toStdString());
        return false;
    }
    return true;
}

std::vector<TrafficCount> TrafficRepository::getCounts(int camera_id, std::time_t from_ts, std::time_t to_ts, int limit) {
    std::vector<TrafficCount> results;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return results;

    std::string sql = "SELECT id, camera_id, roi_id, direction, vehicle_type, count, period_start, period_end, created_at FROM traffic_counts WHERE 1=1 ";
    if (camera_id != -1) sql += " AND camera_id = " + std::to_string(camera_id);
    if (from_ts > 0) sql += " AND period_start >= " + std::to_string(from_ts);
    if (to_ts > 0) sql += " AND period_start <= " + std::to_string(to_ts);
    sql += " ORDER BY period_start DESC LIMIT " + std::to_string(limit);

    QSqlQuery query(db);
    if (!query.exec(QString::fromStdString(sql))) {
        LOG_ERROR("TrafficRepository::getCounts failed: {}", query.lastError().text().toStdString());
        return results;
    }

    while (query.next()) {
        TrafficCount tc;
        tc.id = query.value(0).toInt();
        tc.camera_id = query.value(1).toInt();
        tc.roi_id = query.value(2).toInt();
        tc.direction = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        tc.vehicle_type = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        tc.count = query.value(5).toInt();
        tc.period_start = query.value(6).toLongLong();
        tc.period_end = query.value(7).toLongLong();
        tc.created_at = query.value(8).toLongLong();
        results.push_back(tc);
    }
    return results;
}

TrafficSummary TrafficRepository::getSummary(int camera_id) {
    TrafficSummary s;
    s.camera_id = camera_id;

    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return s;

    // Start of current day (Unix ts)
    std::time_t now = std::time(nullptr);
    struct tm* t = std::localtime(&now);
    t->tm_hour = 0; t->tm_min = 0; t->tm_sec = 0;
    std::time_t day_start = std::mktime(t);

    // Sum In/Out for today
    {
        QSqlQuery query(db);
        query.prepare("SELECT direction, SUM(count) FROM traffic_counts WHERE camera_id = ? AND period_start >= ? GROUP BY direction");
        query.bindValue(0, camera_id);
        query.bindValue(1, static_cast<qlonglong>(day_start));

        if (query.exec()) {
            while (query.next()) {
                std::string dir = query.value(0).isNull() ? "" : query.value(0).toString().toStdString();
                int count = query.value(1).toInt();
                if (dir == "in") s.total_in += count;
                else if (dir == "out") s.total_out += count;
            }
        }
    }
    s.total_today = s.total_in + s.total_out;

    // Peak Hour (group by hour part of timestamp)
    {
        QSqlQuery query(db);
        query.prepare(
            "SELECT strftime('%H', datetime(period_start, 'unixepoch')) as hr, SUM(count) as total "
            "FROM traffic_counts WHERE camera_id = ? AND period_start >= ? GROUP BY hr ORDER BY total DESC LIMIT 1"
        );
        query.bindValue(0, camera_id);
        query.bindValue(1, static_cast<qlonglong>(day_start));

        if (query.exec()) {
            if (query.next()) {
                QString hr_str = query.value(0).toString();
                if (!hr_str.isEmpty()) s.peak_hour = hr_str.toInt();
                s.peak_count = query.value(1).toInt();
            }
        }
    }

    return s;
}

bool TrafficRepository::pruneOld(std::time_t before_ts) {
    auto& db_mgr = DbManager::getInstance();
    std::string sql = "DELETE FROM traffic_counts WHERE period_start < " + std::to_string(before_ts);
    return db_mgr.execute(sql);
}

} // namespace database
} // namespace vms
