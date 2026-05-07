#include "database/anpr_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>

namespace vms {
namespace database {

std::vector<LicensePlate> ANPRRepository::getAllPlates(int limit, int offset) {
    std::vector<LicensePlate> plates;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, plate_number, vehicle_type, color, image_path, camera_id, confidence, detected_at "
                  "FROM license_plates ORDER BY detected_at DESC LIMIT ? OFFSET ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, limit);
    query.bindValue(1, offset);

    if (!query.exec()) {
        LOG_ERROR("SQL error (getAllPlates): {}", query.lastError().text().toStdString());
        return plates;
    }

    while (query.next()) {
        LicensePlate p;
        p.id = query.value(0).toInt();
        p.plate_number = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        p.vehicle_type = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        p.color = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        p.image_path = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        p.camera_id = query.value(5).toInt();
        p.confidence = static_cast<float>(query.value(6).toDouble());
        p.detected_at = query.value(7).toLongLong();
        plates.push_back(p);
    }

    return plates;
}

int ANPRRepository::getPlateCount() {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT COUNT(*) FROM license_plates";

    QSqlQuery query(db);
    query.prepare(sql);

    if (!query.exec()) {
        LOG_ERROR("SQL error (getPlateCount): {}", query.lastError().text().toStdString());
        return 0;
    }

    if (query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

std::vector<LicensePlate> ANPRRepository::getPlatesByCamera(int camera_id, int limit) {
    std::vector<LicensePlate> plates;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, plate_number, vehicle_type, color, image_path, camera_id, confidence, detected_at "
                  "FROM license_plates WHERE camera_id = ? ORDER BY detected_at DESC LIMIT ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, camera_id);
    query.bindValue(1, limit);

    if (!query.exec()) {
        LOG_ERROR("SQL error (getPlatesByCamera): {}", query.lastError().text().toStdString());
        return plates;
    }

    while (query.next()) {
        LicensePlate p;
        p.id = query.value(0).toInt();
        p.plate_number = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        p.vehicle_type = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        p.color = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        p.image_path = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        p.camera_id = query.value(5).toInt();
        p.confidence = static_cast<float>(query.value(6).toDouble());
        p.detected_at = query.value(7).toLongLong();
        plates.push_back(p);
    }

    return plates;
}

std::optional<LicensePlate> ANPRRepository::getPlateById(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, plate_number, vehicle_type, color, image_path, camera_id, confidence, detected_at "
                  "FROM license_plates WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("SQL error (getPlateById): {}", query.lastError().text().toStdString());
        return std::nullopt;
    }

    if (query.next()) {
        LicensePlate p;
        p.id = query.value(0).toInt();
        p.plate_number = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        p.vehicle_type = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        p.color = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        p.image_path = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        p.camera_id = query.value(5).toInt();
        p.confidence = static_cast<float>(query.value(6).toDouble());
        p.detected_at = query.value(7).toLongLong();
        return p;
    }

    return std::nullopt;
}

bool ANPRRepository::insertPlate(const LicensePlate& plate) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "INSERT INTO license_plates (plate_number, vehicle_type, color, image_path, camera_id, confidence, detected_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?)";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, QString::fromStdString(plate.plate_number));
    query.bindValue(1, QString::fromStdString(plate.vehicle_type));
    query.bindValue(2, QString::fromStdString(plate.color));
    query.bindValue(3, QString::fromStdString(plate.image_path));
    query.bindValue(4, plate.camera_id);
    query.bindValue(5, static_cast<double>(plate.confidence));
    query.bindValue(6, static_cast<qint64>(plate.detected_at));

    if (!query.exec()) {
        LOG_ERROR("SQL error (insertPlate): {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

bool ANPRRepository::deletePlate(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "DELETE FROM license_plates WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("SQL error (deletePlate): {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

bool ANPRRepository::clearAllPlates() {
    return DbManager::getInstance().execute("DELETE FROM license_plates");
}

std::vector<LicensePlate> ANPRRepository::searchPlates(const std::string& query_str) {
    std::vector<LicensePlate> plates;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, plate_number, vehicle_type, color, image_path, camera_id, confidence, detected_at "
                  "FROM license_plates WHERE plate_number LIKE ? ORDER BY detected_at DESC";

    QSqlQuery query(db);
    query.prepare(sql);
    QString like_query = "%" + QString::fromStdString(query_str) + "%";
    query.bindValue(0, like_query);

    if (!query.exec()) {
        LOG_ERROR("SQL error (searchPlates): {}", query.lastError().text().toStdString());
        return plates;
    }

    while (query.next()) {
        LicensePlate p;
        p.id = query.value(0).toInt();
        p.plate_number = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        p.vehicle_type = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        p.color = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        p.image_path = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        p.camera_id = query.value(5).toInt();
        p.confidence = static_cast<float>(query.value(6).toDouble());
        p.detected_at = query.value(7).toLongLong();
        plates.push_back(p);
    }

    return plates;
}

} // namespace database
} // namespace vms
