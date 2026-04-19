// ==============================================================
// File: src/api/videowall_controller.cpp
// Video Wall Layout CRUD API
// ==============================================================

#include "api/videowall_controller.h"
#include "database/db_manager.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <ctime>

using json = nlohmann::json;

#ifdef DELETE
#undef DELETE
#endif

namespace vms {
namespace api {

void VideoWallController::registerRoutes(vms::server::VmsApp& app) {
    LOG_INFO("Registering Video Wall routes...");

    // GET /api/videowall/layouts — List all saved layouts
    CROW_ROUTE(app, "/api/videowall/layouts")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

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
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // POST /api/videowall/layouts — Create a new layout
    CROW_ROUTE(app, "/api/videowall/layouts")
    .methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto body = json::parse(req.body);
            std::string name = body.value("name", "Untitled Layout");
            int cols = body.value("grid_cols", 4);
            int rows = body.value("grid_rows", 4);
            std::string cells = body.value("cells", json::array()).dump();
            std::string created_by = body.value("created_by", "admin");

            if (cols < 1 || cols > 8 || rows < 1 || rows > 8) {
                return ApiUtils::createErrorResponse("grid_cols and grid_rows must be 1-8", 400, origin);
            }

            auto& db = database::DbManager::getInstance();
            std::string sql = "INSERT INTO videowall_layouts (name, grid_cols, grid_rows, cells_json, created_by, created_at, updated_at) VALUES (?, ?, ?, ?, ?, strftime('%s','now'), strftime('%s','now'))";
            bool ok = db.executeParameterized(sql, {
                name,
                std::to_string(cols),
                std::to_string(rows),
                cells,
                created_by
            });

            if (!ok) return ApiUtils::createErrorResponse("Failed to create layout", 500, origin);

            int new_id;
            {
                QSqlQuery query(db.getThreadConnection());
                query.exec("SELECT last_insert_rowid()");
                query.next();
                new_id = query.value(0).toInt();
            }

            return ApiUtils::createResponse({
                {"id", new_id},
                {"name", name},
                {"grid_cols", cols},
                {"grid_rows", rows}
            }, 201, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // PUT /api/videowall/layouts/<int> — Update
    CROW_ROUTE(app, "/api/videowall/layouts/<int>")
    .methods(crow::HTTPMethod::Put, crow::HTTPMethod::Options)
    ([](const crow::request& req, int layout_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

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

            sets.push_back("updated_at = strftime('%s','now')");
            params.push_back(std::to_string(layout_id));

            std::string sql = "UPDATE videowall_layouts SET ";
            for (size_t i = 0; i < sets.size(); ++i) {
                sql += sets[i];
                if (i < sets.size() - 1) sql += ", ";
            }
            sql += " WHERE id = ?";

            auto& db = database::DbManager::getInstance();
            bool ok = db.executeParameterized(sql, params);

            if (!ok) return ApiUtils::createErrorResponse("Update failed", 500, origin);
            return ApiUtils::createResponse(json::object(), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // DELETE /api/videowall/layouts/<int>
    CROW_ROUTE(app, "/api/videowall/layouts/<int>")
    .methods(crow::HTTPMethod::Delete)
    ([](const crow::request& req, int layout_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

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
