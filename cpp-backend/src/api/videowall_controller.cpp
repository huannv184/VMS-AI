// ==============================================================
// File: src/api/videowall_controller.cpp
// Video Wall Layout CRUD API
// ==============================================================

#include "api/videowall_controller.h"
#include "database/db_manager.h"
#include "middleware/auth_middleware.h"
#include "server/vms_app.h"
#include "utils/api_utils.h"
#include "utils/config.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <ctime>

using json = nlohmann::json;

namespace {
// BUG-26 / RBAC tightening: previously this helper only verified the request
// was authenticated, which meant a viewer could overwrite shared layouts.
// Now it also enforces the requested module permission so writes go through
// the same RBAC gate as every other mutate API.
std::optional<crow::response> requireVideoWallPerm(
        vms::server::VmsApp& app,
        const crow::request& req,
        vms::api::Permission perm,
        const std::string& origin) {
    if (!vms::Config::getInstance().getAuthConfig().enabled) {
        return std::nullopt;
    }
    auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
    return vms::api::ApiUtils::requirePermission(ctx, perm, origin);
}

std::string callerUsername(vms::server::VmsApp& app, const crow::request& req) {
    if (!vms::Config::getInstance().getAuthConfig().enabled) return "admin";
    try {
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (ctx.user.has_value()) return ctx.user->username;
    } catch (...) {}
    return "unknown";
}
} // namespace

#ifdef DELETE
#undef DELETE
#endif

namespace vms {
namespace api {

void VideoWallController::registerRoutes(vms::server::VmsApp& app) {
    LOG_INFO("Registering Video Wall routes...");

    // GET /api/videowall/layouts — List all saved layouts (VIDEOWALL_READ)
    CROW_ROUTE(app, "/api/videowall/layouts")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        if (auto err = requireVideoWallPerm(app, req, Permission::VIDEOWALL_READ, origin)) return std::move(*err);

        try {
            auto& db = database::DbManager::getInstance();
            QSqlDatabase conn = db.getThreadConnection();
            if (!conn.isOpen()) return ApiUtils::createErrorResponse("DB unavailable", 500, origin);

            json layouts = json::array();
            QSqlQuery query(conn);
            query.prepare("SELECT id, name, grid_cols, grid_rows, cells_json, created_by, created_at, updated_at FROM videowall_layouts ORDER BY updated_at DESC");
            if (query.exec()) {
                while (query.next()) {
                    json layout;
                    layout["id"] = query.value(0).toInt();
                    layout["name"] = query.value(1).toString().toStdString();
                    layout["grid_cols"] = query.value(2).toInt();
                    layout["grid_rows"] = query.value(3).toInt();
                    try {
                        layout["cells"] = json::parse(query.value(4).toString().toStdString());
                    } catch (...) {
                        layout["cells"] = json::array();
                    }
                    std::string created_by = query.value(5).isNull() ? "" : query.value(5).toString().toStdString();
                    layout["created_by"] = created_by;
                    layout["created_at"] = query.value(6).toLongLong();
                    layout["updated_at"] = query.value(7).toLongLong();
                    layouts.push_back(layout);
                }
            }

            return ApiUtils::createResponse({{"layouts", layouts}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // POST /api/videowall/layouts — Create a new layout
    CROW_ROUTE(app, "/api/videowall/layouts")
    .methods(crow::HTTPMethod::Post)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);
        if (auto err = requireVideoWallPerm(app, req, Permission::VIDEOWALL_WRITE, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            std::string name = body.value("name", "Untitled Layout");
            int cols = body.value("grid_cols", 4);
            int rows = body.value("grid_rows", 4);
            std::string cells = body.value("cells", json::array()).dump();
            // Audit attribution must come from the auth context, not the request
            // body — clients used to be able to forge created_by="root" to hide
            // who actually made the change.
            std::string created_by = callerUsername(app, req);

            if (cols < 1 || cols > 8 || rows < 1 || rows > 8) {
                return ApiUtils::createErrorResponse("grid_cols and grid_rows must be 1-8", 400, origin);
            }

            auto& db = database::DbManager::getInstance();
            const std::string now = db.sqlNowEpoch();

            int new_id = -1;
            if (db.isPostgres()) {
                // PostgreSQL: use RETURNING to get new id atomically
                QSqlQuery q(db.getThreadConnection());
                std::string sql = "INSERT INTO videowall_layouts (name, grid_cols, grid_rows, cells_json, created_by, created_at, updated_at) VALUES (?, ?, ?, ?, ?, " + now + ", " + now + ") RETURNING id";
                q.prepare(QString::fromStdString(sql));
                q.addBindValue(QString::fromStdString(name));
                q.addBindValue(cols);
                q.addBindValue(rows);
                q.addBindValue(QString::fromStdString(cells));
                q.addBindValue(QString::fromStdString(created_by));
                if (!q.exec() || !q.next())
                    return ApiUtils::createErrorResponse("Failed to create layout", 500, origin);
                new_id = q.value(0).toInt();
            } else {
                std::string sql = "INSERT INTO videowall_layouts (name, grid_cols, grid_rows, cells_json, created_by, created_at, updated_at) VALUES (?, ?, ?, ?, ?, " + now + ", " + now + ")";
                bool ok = db.executeParameterized(sql, {name, std::to_string(cols), std::to_string(rows), cells, created_by});
                if (!ok) return ApiUtils::createErrorResponse("Failed to create layout", 500, origin);
                QSqlQuery q(db.getThreadConnection());
                q.exec("SELECT last_insert_rowid()");
                q.next();
                new_id = q.value(0).toInt();
            }

            return ApiUtils::createResponse({
                {"id", new_id},
                {"name", name},
                {"grid_cols", cols},
                {"grid_rows", rows}
            }, 201, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 400, origin);
        }
    });

    // PUT /api/videowall/layouts/<int> — Update
    CROW_ROUTE(app, "/api/videowall/layouts/<int>")
    .methods(crow::HTTPMethod::Put, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int layout_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);
        if (auto err = requireVideoWallPerm(app, req, Permission::VIDEOWALL_WRITE, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);

            std::vector<std::string> sets;
            std::vector<std::string> params;

            if (body.contains("name")) {
                sets.push_back("name = ?");
                params.push_back(body["name"].get<std::string>());
            }
            if (body.contains("grid_cols")) {
                sets.push_back("grid_cols = ?");
                params.push_back(std::to_string(body["grid_cols"].get<int>()));
            }
            if (body.contains("grid_rows")) {
                sets.push_back("grid_rows = ?");
                params.push_back(std::to_string(body["grid_rows"].get<int>()));
            }
            if (body.contains("cells")) {
                sets.push_back("cells_json = ?");
                params.push_back(body["cells"].dump());
            }

            if (sets.empty()) {
                return ApiUtils::createErrorResponse("No fields to update", 400, origin);
            }

            auto& db = database::DbManager::getInstance();
            sets.push_back("updated_at = " + db.sqlNowEpoch());
            params.push_back(std::to_string(layout_id));

            std::string sql = "UPDATE videowall_layouts SET ";
            for (size_t i = 0; i < sets.size(); ++i) {
                sql += sets[i];
                if (i < sets.size() - 1) sql += ", ";
            }
            sql += " WHERE id = ?";

            bool ok = db.executeParameterized(sql, params);

            if (!ok) return ApiUtils::createErrorResponse("Update failed", 500, origin);
            return ApiUtils::createResponse(json::object(), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 400, origin);
        }
    });

    // DELETE /api/videowall/layouts/<int>
    CROW_ROUTE(app, "/api/videowall/layouts/<int>")
    .methods(crow::HTTPMethod::Delete)
    ([&app](const crow::request& req, int layout_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);
        if (auto err = requireVideoWallPerm(app, req, Permission::VIDEOWALL_WRITE, origin)) return std::move(*err);

        auto& db = database::DbManager::getInstance();
        bool ok = db.executeParameterized("DELETE FROM videowall_layouts WHERE id = ?",
                                          {std::to_string(layout_id)});
        if (!ok) return ApiUtils::createErrorResponse("Delete failed", 500, origin);
        return ApiUtils::createResponse(json::object(), 200, origin);
    });

    LOG_INFO("Video Wall routes registered");
}

} // namespace api
} // namespace vms
