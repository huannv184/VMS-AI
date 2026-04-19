#pragma once

#include "database/db_manager.h"
#include "database/models.h"
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>

namespace vms {
namespace database {

class PermissionRepository {
public:
    static UserPermission getPermissions(int userId) {
        UserPermission p;
        p.user_id = userId;

        QSqlDatabase db = DbManager::getInstance().getThreadConnection();
        if (!db.isOpen()) return p;

        QSqlQuery query(db);
        query.prepare(
            "SELECT allowed_cameras, allowed_sites, can_view_live, can_view_playback, can_manage_settings, can_ptz "
            "FROM permissions WHERE user_id = ?"
        );
        query.bindValue(0, userId);

        if (!query.exec()) return p;

        if (query.next()) {
            try {
                std::string cams_str = query.value(0).isNull() ? "[]" : query.value(0).toString().toStdString();
                auto cams_json = nlohmann::json::parse(cams_str);
                p.allowed_camera_ids = cams_json.get<std::vector<int>>();

                std::string sites_str = query.value(1).isNull() ? "[]" : query.value(1).toString().toStdString();
                auto sites_json = nlohmann::json::parse(sites_str);
                p.allowed_site_ids = sites_json.get<std::vector<int>>();

                p.can_view_live = query.value(2).toInt() != 0;
                p.can_view_playback = query.value(3).toInt() != 0;
                p.can_manage_settings = query.value(4).toInt() != 0;
                p.can_ptz = query.value(5).toInt() != 0;
            } catch (...) {}
        }
        return p;
    }

    static bool setPermissions(const UserPermission& p) {
        auto& db_mgr = DbManager::getInstance();

        std::string cams_json = nlohmann::json(p.allowed_camera_ids).dump();
        std::string sites_json = nlohmann::json(p.allowed_site_ids).dump();

        std::string q = "INSERT OR REPLACE INTO permissions (user_id, allowed_cameras, allowed_sites, can_view_live, can_view_playback, can_manage_settings, can_ptz) VALUES (?, ?, ?, ?, ?, ?, ?)";
        return db_mgr.executeParameterized(q, {
            std::to_string(p.user_id),
            cams_json,
            sites_json,
            p.can_view_live ? "1" : "0",
            p.can_view_playback ? "1" : "0",
            p.can_manage_settings ? "1" : "0",
            p.can_ptz ? "1" : "0"
        });
    }
};

} // namespace database
} // namespace vms
