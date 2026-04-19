#pragma once

#include "database/db_manager.h"
#include "database/models.h"
#include "utils/logger.h"
#include <vector>
#include <optional>

#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>

namespace vms {
namespace database {

class SiteRepository {
public:
    static std::vector<Site> getAllSites() {
        std::vector<Site> sites;

        QSqlDatabase db = DbManager::getInstance().getThreadConnection();
        if (!db.isOpen()) return sites;

        QSqlQuery query(db);
        if (!query.exec("SELECT id, name, address, parent_site_id, description, created_at FROM sites")) {
            LOG_ERROR("SiteRepository::getAllSites failed: {}", query.lastError().text().toStdString());
            return sites;
        }

        while (query.next()) {
            Site s;
            s.id = query.value(0).toInt();
            s.name = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
            s.address = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
            s.parent_site_id = query.value(3).toInt();
            s.description = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
            s.created_at = query.value(5).toLongLong();
            sites.push_back(s);
        }
        return sites;
    }

    static bool createSite(Site& site) {
        QSqlDatabase db = DbManager::getInstance().getThreadConnection();
        if (!db.isOpen()) return false;

        site.created_at = std::time(nullptr);

        QSqlQuery query(db);
        query.prepare("INSERT INTO sites (name, address, parent_site_id, description, created_at) VALUES (?, ?, ?, ?, ?)");
        query.bindValue(0, QString::fromStdString(site.name));
        query.bindValue(1, QString::fromStdString(site.address));
        query.bindValue(2, site.parent_site_id);
        query.bindValue(3, QString::fromStdString(site.description));
        query.bindValue(4, static_cast<qlonglong>(site.created_at));

        if (!query.exec()) {
            LOG_ERROR("SiteRepository::createSite failed: {}", query.lastError().text().toStdString());
            return false;
        }

        site.id = query.lastInsertId().toInt();
        return true;
    }

    static bool deleteSite(int id) {
        auto& db_mgr = DbManager::getInstance();
        // Also nullify camera site IDs
        db_mgr.executeParameterized("UPDATE cameras SET site_id = -1 WHERE site_id = ?", {std::to_string(id)});
        return db_mgr.executeParameterized("DELETE FROM sites WHERE id = ?", {std::to_string(id)});
    }
};

} // namespace database
} // namespace vms
