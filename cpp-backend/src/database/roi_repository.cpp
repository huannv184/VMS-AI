#include "database/roi_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"

#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>

#include <ctime>

namespace vms {
namespace database {

std::vector<ROI> ROIRepository::getAllROIs() {
    std::vector<ROI> rois;

    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return rois;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, camera_id, name, roi_type, points_json, is_active, created_at FROM rois ORDER BY id");

    if (!query.exec()) {
        return rois;
    }

    while (query.next()) {
        ROI roi;
        roi.id = query.value(0).toInt();
        roi.camera_id = query.value(1).toInt();
        roi.name = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        roi.roi_type = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        roi.points_json = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        roi.enabled = query.value(5).toInt() != 0;
        roi.created_at = query.value(6).toLongLong();

        rois.push_back(roi);
    }

    return rois;
}

std::vector<ROI> ROIRepository::getCameraROIs(int camera_id) {
    std::vector<ROI> rois;

    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return rois;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, camera_id, name, roi_type, points_json, is_active, created_at FROM rois WHERE camera_id = ? ORDER BY id");
    query.bindValue(0, camera_id);

    if (!query.exec()) {
        return rois;
    }

    while (query.next()) {
        ROI roi;
        roi.id = query.value(0).toInt();
        roi.camera_id = query.value(1).toInt();
        roi.name = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        roi.roi_type = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        roi.points_json = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        roi.enabled = query.value(5).toInt() != 0;
        roi.created_at = query.value(6).toLongLong();

        rois.push_back(roi);
    }

    return rois;
}

std::optional<ROI> ROIRepository::getROIById(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return std::nullopt;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, camera_id, name, roi_type, points_json, is_active, created_at FROM rois WHERE id = ?");
    query.bindValue(0, id);

    if (!query.exec()) {
        return std::nullopt;
    }

    if (query.next()) {
        ROI roi;
        roi.id = query.value(0).toInt();
        roi.camera_id = query.value(1).toInt();
        roi.name = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        roi.roi_type = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        roi.points_json = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        roi.enabled = query.value(5).toInt() != 0;
        roi.created_at = query.value(6).toLongLong();

        return roi;
    }

    return std::nullopt;
}

int ROIRepository::insertROI(const ROI& roi) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return -1;
    }

    QSqlQuery query(db);
    query.prepare("INSERT INTO rois (camera_id, name, roi_type, points_json, is_active, created_at) VALUES (?, ?, ?, ?, ?, ?)");

    std::time_t now = std::time(nullptr);

    query.bindValue(0, roi.camera_id);
    query.bindValue(1, QString::fromStdString(roi.name));
    query.bindValue(2, QString::fromStdString(roi.roi_type));
    query.bindValue(3, QString::fromStdString(roi.points_json));
    query.bindValue(4, roi.enabled ? 1 : 0);
    query.bindValue(5, static_cast<qint64>(now));

    if (!query.exec()) {
        return -1;
    }

    // FIX: Return the auto-generated row ID so callers can include it in responses
    return query.lastInsertId().toInt();
}

bool ROIRepository::updateROI(const ROI& roi) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return false;
    }

    QSqlQuery query(db);
    query.prepare("UPDATE rois SET name = ?, roi_type = ?, points_json = ?, is_active = ? WHERE id = ?");

    query.bindValue(0, QString::fromStdString(roi.name));
    query.bindValue(1, QString::fromStdString(roi.roi_type));
    query.bindValue(2, QString::fromStdString(roi.points_json));
    query.bindValue(3, roi.enabled ? 1 : 0);
    query.bindValue(4, roi.id);

    if (!query.exec()) {
        return false;
    }

    return true;
}

bool ROIRepository::deleteROI(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        return false;
    }

    QSqlQuery query(db);
    query.prepare("DELETE FROM rois WHERE id = ?");
    query.bindValue(0, id);

    if (!query.exec()) {
        return false;
    }

    return true;
}

} // namespace database
} // namespace vms
