#include "database/camera_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include "database/permission_repository.h"
#include "database/user_repository.h"
#include <nlohmann/json.hpp>
#include <ctime>
#include <set>

#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>

namespace vms {
namespace database {

// Helper for mapping
Camera CameraRepository::mapRowToCamera(QSqlQuery& query) {
    Camera camera;
    camera.id = query.value(0).toInt();
    camera.name = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
    camera.rtsp_url = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
    camera.sub_stream_url = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
    camera.description = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
    camera.is_active = query.value(5).toInt() != 0;
    camera.zmq_port = query.value(6).toInt();

    QString ai_cfg = query.value(7).toString();
    camera.ai_config = (ai_cfg.isEmpty()) ? "{\"yolo\":true,\"face\":true,\"lpr\":true,\"fire\":true}" : ai_cfg.toStdString();

    camera.created_at = query.value(8).toLongLong();
    camera.updated_at = query.value(9).toLongLong();
    camera.advanced_config = query.value(10).isNull() ? "{}" : query.value(10).toString().toStdString();

    camera.motion_sensitivity = query.value(12).toDouble();
    camera.site_id = query.value(13).toInt();

    return camera;
}

std::vector<Camera> CameraRepository::getAllCameras() {
    std::vector<Camera> cameras;

    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return cameras;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, name, rtsp_url, sub_stream_url, description, is_active, zmq_port, ai_config, created_at, updated_at, advanced_config, motion_detection_enabled, motion_sensitivity, site_id FROM cameras ORDER BY id");

    if (!query.exec()) {
        LOG_ERROR("Failed to execute query: {}", query.lastError().text().toStdString());
        return cameras;
    }

    while (query.next()) {
        cameras.push_back(mapRowToCamera(query));
    }

    return cameras;
}

std::optional<Camera> CameraRepository::getCameraById(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return std::nullopt;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, name, rtsp_url, sub_stream_url, description, is_active, zmq_port, ai_config, created_at, updated_at, advanced_config, motion_detection_enabled, motion_sensitivity, site_id FROM cameras WHERE id = ?");
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute query: {}", query.lastError().text().toStdString());
        return std::nullopt;
    }

    if (query.next()) {
        return mapRowToCamera(query);
    }

    return std::nullopt;
}

bool CameraRepository::insertCamera(Camera& camera) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return false;
    }

    QSqlQuery query(db);
    query.prepare("INSERT INTO cameras (name, rtsp_url, sub_stream_url, description, is_active, zmq_port, ai_config, created_at, updated_at, advanced_config, motion_detection_enabled, motion_sensitivity, site_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    std::time_t now = std::time(nullptr);

    query.bindValue(0, QString::fromStdString(camera.name));
    query.bindValue(1, QString::fromStdString(camera.rtsp_url));
    query.bindValue(2, QString::fromStdString(camera.sub_stream_url));
    query.bindValue(3, QString::fromStdString(camera.description));
    query.bindValue(4, camera.is_active ? 1 : 0);
    query.bindValue(5, camera.zmq_port);

    QString ai_config_val = camera.ai_config.empty()
        ? QString("{\"yolo\":true,\"face\":true,\"lpr\":true,\"fire\":true}")
        : QString::fromStdString(camera.ai_config);
    query.bindValue(6, ai_config_val);

    query.bindValue(7, static_cast<qint64>(now));
    query.bindValue(8, static_cast<qint64>(now));
    query.bindValue(9, QString::fromStdString(camera.advanced_config));
    query.bindValue(10, camera.motion_detection_enabled ? 1 : 0);
    query.bindValue(11, camera.motion_sensitivity);
    query.bindValue(12, camera.site_id);

    if (!query.exec()) {
        LOG_ERROR("Failed to insert camera: {}", query.lastError().text().toStdString());
        return false;
    }

    camera.id = query.lastInsertId().toInt();
    LOG_INFO("Camera inserted: {} (ID: {})", camera.name, camera.id);
    return true;
}

bool CameraRepository::updateCamera(const Camera& camera) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return false;
    }

    QSqlQuery query(db);
    query.prepare("UPDATE cameras SET name = ?, rtsp_url = ?, sub_stream_url = ?, description = ?, is_active = ?, zmq_port = ?, ai_config = ?, updated_at = ?, advanced_config = ?, motion_detection_enabled = ?, motion_sensitivity = ?, site_id = ? WHERE id = ?");

    std::time_t now = std::time(nullptr);

    query.bindValue(0, QString::fromStdString(camera.name));
    query.bindValue(1, QString::fromStdString(camera.rtsp_url));
    query.bindValue(2, QString::fromStdString(camera.sub_stream_url));
    query.bindValue(3, QString::fromStdString(camera.description));
    query.bindValue(4, camera.is_active ? 1 : 0);
    query.bindValue(5, camera.zmq_port);

    QString ai_config_val = camera.ai_config.empty()
        ? QString("{\"yolo\":true,\"face\":true,\"lpr\":true,\"fire\":true}")
        : QString::fromStdString(camera.ai_config);
    query.bindValue(6, ai_config_val);

    query.bindValue(7, static_cast<qint64>(now));
    query.bindValue(8, QString::fromStdString(camera.advanced_config));
    query.bindValue(9, camera.motion_detection_enabled ? 1 : 0);
    query.bindValue(10, camera.motion_sensitivity);
    query.bindValue(11, camera.site_id);
    query.bindValue(12, camera.id);

    if (!query.exec()) {
        LOG_ERROR("Failed to update camera: {}", query.lastError().text().toStdString());
        return false;
    }

    LOG_INFO("Camera updated: {} (ID: {})", camera.name, camera.id);
    return true;
}

bool CameraRepository::deleteCamera(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return false;
    }

    QSqlQuery query(db);
    query.prepare("DELETE FROM cameras WHERE id = ?");
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to delete camera: {}", query.lastError().text().toStdString());
        return false;
    }

    LOG_INFO("Camera deleted: ID {}", id);
    return true;
}

std::vector<Camera> CameraRepository::getActiveCameras() {
    std::vector<Camera> cameras;

    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return cameras;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, name, rtsp_url, sub_stream_url, description, is_active, zmq_port, ai_config, created_at, updated_at, advanced_config, motion_detection_enabled, motion_sensitivity, site_id FROM cameras WHERE is_active = 1 ORDER BY id");

    if (!query.exec()) {
        LOG_ERROR("Failed to execute query: {}", query.lastError().text().toStdString());
        return cameras;
    }

    while (query.next()) {
        cameras.push_back(mapRowToCamera(query));
    }

    return cameras;
}

std::optional<Camera> CameraRepository::findByNameOrRtsp(const std::string& name, const std::string& rtsp_url,
                                                         std::optional<int> exclude_id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return std::nullopt;
    }

    QSqlQuery query(db);
    query.prepare(
        "SELECT id, name, rtsp_url, sub_stream_url, description, is_active, zmq_port, ai_config, created_at, updated_at, advanced_config, motion_detection_enabled, motion_sensitivity, site_id "
        "FROM cameras WHERE (LOWER(name) = LOWER(?) OR rtsp_url = ?) AND (? IS NULL OR id != ?) LIMIT 1");

    query.bindValue(0, QString::fromStdString(name));
    query.bindValue(1, QString::fromStdString(rtsp_url));
    if (exclude_id.has_value()) {
        query.bindValue(2, exclude_id.value());
        query.bindValue(3, exclude_id.value());
    } else {
        query.bindValue(2, QVariant(QVariant::Int));
        query.bindValue(3, QVariant(QVariant::Int));
    }

    if (!query.exec()) {
        LOG_ERROR("Failed to execute query: {}", query.lastError().text().toStdString());
        return std::nullopt;
    }

    if (query.next()) {
        return mapRowToCamera(query);
    }

    return std::nullopt;
}

std::vector<Camera> CameraRepository::getFilteredCameras(int userId) {
    // 1. Check if user is admin
    UserRepository user_repo;
    auto user_opt = user_repo.getUserById(userId);
    if (user_opt && user_opt->role_id == 1) {
        return getAllCameras();
    }

    // 2. Fetch permissions
    auto permissions = PermissionRepository::getPermissions(userId);
    std::vector<Camera> filtered;

    bool has_cams = !permissions.allowed_camera_ids.empty();
    bool has_sites = !permissions.allowed_site_ids.empty();

    if (!has_cams && !has_sites) {
        // No permissions defined means no cameras visible for non-admins
        return filtered;
    }

    // 3. Build Query
    std::string sql = "SELECT id, name, rtsp_url, sub_stream_url, description, is_active, zmq_port, ai_config, created_at, updated_at, advanced_config, motion_detection_enabled, motion_sensitivity, site_id FROM cameras WHERE 1=1 ";

    sql += " AND (";
    if (has_cams) {
        sql += "id IN (";
        for (size_t i = 0; i < permissions.allowed_camera_ids.size(); ++i) {
            sql += std::to_string(permissions.allowed_camera_ids[i]) + (i == permissions.allowed_camera_ids.size() - 1 ? "" : ",");
        }
        sql += ")";
    }
    if (has_cams && has_sites) sql += " OR ";
    if (has_sites) {
        sql += "site_id IN (";
        for (size_t i = 0; i < permissions.allowed_site_ids.size(); ++i) {
            sql += std::to_string(permissions.allowed_site_ids[i]) + (i == permissions.allowed_site_ids.size() - 1 ? "" : ",");
        }
        sql += ")";
    }
    sql += ")";

    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) {
        LOG_ERROR("Database not initialized");
        return filtered;
    }

    QSqlQuery query(db);
    query.prepare(QString::fromStdString(sql));

    if (!query.exec()) {
        LOG_ERROR("Failed to prepare filtered cameras statement: {}", query.lastError().text().toStdString());
        return filtered;
    }

    while (query.next()) {
        filtered.push_back(mapRowToCamera(query));
    }

    return filtered;
}

} // namespace database
} // namespace vms
