#include "api/alert_controller.h"
#include "utils/api_utils.h"
#include "database/db_manager.h"
#include "core/alert_manager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using vms::api::Permission;

#ifdef DELETE
#undef DELETE
#endif

namespace vms {
namespace api {

void AlertController::registerRoutes(vms::server::VmsApp& app) {
    // GET/POST /api/alerts/rules
    CROW_ROUTE(app, "/api/alerts/rules")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (req.method == crow::HTTPMethod::POST) {
            if (auto err = ApiUtils::requirePermission(ctx, Permission::ALERT_WRITE, origin)) return std::move(*err);
        }

        if (req.method == crow::HTTPMethod::GET) {
            try {
                auto& db = vms::database::DbManager::getInstance();
                QSqlDatabase conn = db.getThreadConnection();
                QSqlQuery query(conn);
                query.prepare("SELECT id, camera_id, event_type, severity, action_type, recipient, is_enabled FROM alert_rules");
                
                json rules = json::array();
                if (query.exec()) {
                    while (query.next()) {
                        json r;
                        r["id"] = query.value(0).toInt();
                        r["camera_id"] = query.value(1).toInt();
                        r["event_type"] = query.value(2).toString().toStdString();
                        r["severity"] = query.value(3).toString().toStdString();
                        r["action_type"] = query.value(4).toString().toStdString();
                        r["recipient"] = query.value(5).toString().toStdString();
                        r["is_enabled"] = query.value(6).toInt() != 0;
                        rules.push_back(r);
                    }
                }
                return ApiUtils::createResponse({{"rules", rules}}, 200, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        } else if (req.method == crow::HTTPMethod::POST) {
            try {
                auto data = json::parse(req.body);
                auto& db = vms::database::DbManager::getInstance();
                QSqlDatabase conn = db.getThreadConnection();
                QSqlQuery query(conn);
                query.prepare("INSERT INTO alert_rules (camera_id, event_type, severity, action_type, recipient, is_enabled) VALUES (?, ?, ?, ?, ?, ?)");
                query.bindValue(0, data.value("camera_id", -1));
                query.bindValue(1, QString::fromStdString(data.value("event_type", "*")));
                query.bindValue(2, QString::fromStdString(data.value("severity", "high")));
                query.bindValue(3, QString::fromStdString(data.value("action_type", "email")));
                query.bindValue(4, QString::fromStdString(data.value("recipient", "")));
                query.bindValue(5, data.value("is_enabled", true) ? 1 : 0);
                
                if (query.exec()) {
                    vms::core::AlertManager::getInstance().refreshRules();
                    return ApiUtils::createResponse(json::object(), 201, origin);
                }
                return ApiUtils::createErrorResponse(query.lastError().text().toStdString(), 500, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        }
        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // DELETE /api/alerts/rules/:id
    CROW_ROUTE(app, "/api/alerts/rules/<int>")
    .methods(crow::HTTPMethod::DELETE, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ALERT_WRITE, origin)) return std::move(*err);

        try {
            auto& db = vms::database::DbManager::getInstance();
            QSqlDatabase conn = db.getThreadConnection();
            QSqlQuery query(conn);
            query.prepare("DELETE FROM alert_rules WHERE id = ?");
            query.bindValue(0, id);
            
            if (query.exec()) {
                vms::core::AlertManager::getInstance().refreshRules();
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
            return ApiUtils::createErrorResponse(query.lastError().text().toStdString(), 500, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });
}

} // namespace api
} // namespace vms
