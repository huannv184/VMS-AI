// ==============================================================
// File: src/api/counter_controller.cpp
// /api/counter/lines — CRUD for the counting_lines table that
// PeopleCountTracker consults each frame. Every mutation triggers
// PeopleCountTracker::reloadFromDb() so the in-memory cache stays
// in sync (next frame sees the new geometry).
//
// Coordinates: ax/ay/bx/by are normalized 0..1 (fraction of the
// camera frame). Frontend draws on a percent grid; bridge here.
// ==============================================================

#include "api/counter_controller.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/people_count_tracker.h"
#include "database/db_manager.h"
#include "middleware/auth_middleware.h"
#include "utils/api_utils.h"
#include "utils/config.h"
#include "utils/logger.h"

#ifdef DELETE
#undef DELETE
#endif

using json = nlohmann::json;

namespace vms::api {

namespace {

std::optional<crow::response> requireCounterAdmin(
        vms::server::VmsApp& app,
        const crow::request& req,
        const std::string& origin) {
    if (!vms::Config::getInstance().getAuthConfig().enabled) {
        return std::nullopt;
    }
    auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
    return ApiUtils::requireAdmin(ctx, origin);
}

// GET on counting_lines was originally unauthenticated. The line geometry
// (entry/exit coordinates per camera) is operationally sensitive — knowing
// where the people-counting tripwires sit lets an outsider plan to walk
// around them. Gate behind ANALYTICS_READ; admin/operator/viewer all qualify.
std::optional<crow::response> requireCounterRead(
        vms::server::VmsApp& app,
        const crow::request& req,
        const std::string& origin) {
    if (!vms::Config::getInstance().getAuthConfig().enabled) {
        return std::nullopt;
    }
    auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
    return ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin);
}

// Validate normalized coordinate; allow tiny float slop from the UI.
bool isNormalized(float v) {
    return v >= -0.001f && v <= 1.001f;
}

bool validateLineBody(const json& body, std::string* err) {
    if (!body.is_object()) { *err = "body must be a JSON object"; return false; }
    if (!body.contains("camera_id") || !body["camera_id"].is_number_integer()) {
        *err = "camera_id required (integer)"; return false;
    }
    for (const char* k : {"ax", "ay", "bx", "by"}) {
        if (!body.contains(k) || !body[k].is_number()) {
            *err = std::string{k} + " required (number 0..1)"; return false;
        }
        if (!isNormalized(body[k].get<float>())) {
            *err = std::string{k} + " must be in [0, 1]"; return false;
        }
    }
    if (body.contains("object_classes") && !body["object_classes"].is_array()) {
        *err = "object_classes must be array of strings"; return false;
    }
    return true;
}

json rowToJson(QSqlQuery& q) {
    return {
        {"id",                 q.value(0).toInt()},
        {"camera_id",          q.value(1).toInt()},
        {"name",               q.value(2).toString().toStdString()},
        {"ax",                 q.value(3).toFloat()},
        {"ay",                 q.value(4).toFloat()},
        {"bx",                 q.value(5).toFloat()},
        {"by",                 q.value(6).toFloat()},
        {"direction_a_label",  q.value(7).toString().toStdString()},
        {"direction_b_label",  q.value(8).toString().toStdString()},
        {"object_classes_json",q.value(9).toString().toStdString()},
        {"enabled",            q.value(10).toInt() == 1}
    };
}

} // namespace

void CounterController::registerRoutes(vms::server::VmsApp& app) {

    // ─────────────────────────────────────────────────────────────────────
    // GET /api/counter/lines?camera_id=<id>
    //   When camera_id absent, returns all lines.
    // POST /api/counter/lines  body:{camera_id, name?, ax,ay,bx,by,
    //                                direction_a_label?, direction_b_label?,
    //                                object_classes?, enabled?}
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/counter/lines")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (req.method == crow::HTTPMethod::Get) {
            if (auto err = requireCounterRead(app, req, origin)) return std::move(*err);
        }
        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Get) {
                QSqlQuery q(db);
                if (req.url_params.get("camera_id")) {
                    // Validate camera_id before binding. Pre-fix used std::atoi
                    // which returns 0 on garbage input — a typo like
                    // ?camera_id=abc would silently return cameras with id=0
                    // instead of HTTP 400. Pattern from anpr_controller:50-54.
                    int cam_id = 0;
                    try {
                        cam_id = std::stoi(req.url_params.get("camera_id"));
                    } catch (const std::exception&) {
                        return ApiUtils::createErrorResponse("Invalid query parameter: camera_id must be an integer", 400, origin);
                    }
                    q.prepare("SELECT id, camera_id, name, ax, ay, bx, by, "
                              "       direction_a_label, direction_b_label, "
                              "       object_classes_json, enabled "
                              "FROM counting_lines WHERE camera_id = ? ORDER BY id ASC");
                    q.bindValue(0, cam_id);
                } else {
                    q.prepare("SELECT id, camera_id, name, ax, ay, bx, by, "
                              "       direction_a_label, direction_b_label, "
                              "       object_classes_json, enabled "
                              "FROM counting_lines ORDER BY camera_id, id ASC");
                }
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                json arr = json::array();
                while (q.next()) arr.push_back(rowToJson(q));
                return ApiUtils::createResponse({{"lines", arr}}, 200, origin);
            }

            // POST → create. Admin only.
            if (auto err = requireCounterAdmin(app, req, origin)) return std::move(*err);

            auto body = json::parse(req.body);
            std::string verr;
            if (!validateLineBody(body, &verr)) {
                return ApiUtils::createErrorResponse(verr, 400, origin);
            }

            json oc = body.value("object_classes", json::array({"person"}));
            QSqlQuery q(db);
            q.prepare("INSERT INTO counting_lines "
                      "(camera_id, name, ax, ay, bx, by, "
                      " direction_a_label, direction_b_label, "
                      " object_classes_json, enabled) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
            q.bindValue(0, body.value("camera_id", 0));
            q.bindValue(1, QString::fromStdString(body.value("name", std::string{})));
            q.bindValue(2, body.value("ax", 0.0f));
            q.bindValue(3, body.value("ay", 0.0f));
            q.bindValue(4, body.value("bx", 0.0f));
            q.bindValue(5, body.value("by", 0.0f));
            q.bindValue(6, QString::fromStdString(body.value("direction_a_label", std::string{"in"})));
            q.bindValue(7, QString::fromStdString(body.value("direction_b_label", std::string{"out"})));
            q.bindValue(8, QString::fromStdString(oc.dump()));
            q.bindValue(9, body.value("enabled", true) ? 1 : 0);

            if (!q.exec()) {
                return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
            }
            const int new_id = q.lastInsertId().toInt();

            // Hot reload so the next frame sees the new line.
            vms::core::PeopleCountTracker::getInstance().reloadFromDb();

            return ApiUtils::createResponse(
                {{"id", new_id}, {"camera_id", body.value("camera_id", 0)}}, 201, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // PUT/DELETE /api/counter/lines/<int>
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/counter/lines/<int>")
    .methods(crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireCounterAdmin(app, req, origin)) return std::move(*err);

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Delete) {
                QSqlQuery q(db);
                q.prepare("DELETE FROM counting_lines WHERE id = ?");
                q.bindValue(0, id);
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                vms::core::PeopleCountTracker::getInstance().reloadFromDb();
                return ApiUtils::createResponse({{"deleted", true}, {"id", id}}, 200, origin);
            }

            // PUT — partial update. Only fields present in the body are touched.
            auto body = json::parse(req.body);
            // Build dynamic SET clause; reject if no editable field present.
            std::vector<std::string> sets;
            std::vector<QVariant>    binds;
            const auto setStr = [&](const char* k) {
                if (body.contains(k) && body[k].is_string()) {
                    sets.push_back(std::string{k} + " = ?");
                    binds.push_back(QString::fromStdString(body[k].get<std::string>()));
                }
            };
            const auto setNum = [&](const char* k) {
                if (body.contains(k) && body[k].is_number()) {
                    if (!isNormalized(body[k].get<float>())) {
                        throw std::runtime_error(std::string{k} + " must be in [0, 1]");
                    }
                    sets.push_back(std::string{k} + " = ?");
                    binds.push_back(body[k].get<float>());
                }
            };
            setStr("name");
            setNum("ax"); setNum("ay"); setNum("bx"); setNum("by");
            setStr("direction_a_label");
            setStr("direction_b_label");
            if (body.contains("object_classes") && body["object_classes"].is_array()) {
                sets.push_back("object_classes_json = ?");
                binds.push_back(QString::fromStdString(body["object_classes"].dump()));
            }
            if (body.contains("enabled") && body["enabled"].is_boolean()) {
                sets.push_back("enabled = ?");
                binds.push_back(body["enabled"].get<bool>() ? 1 : 0);
            }
            if (sets.empty()) {
                return ApiUtils::createErrorResponse("no editable fields provided", 400, origin);
            }

            std::string sql = "UPDATE counting_lines SET ";
            for (size_t i = 0; i < sets.size(); ++i) {
                if (i) sql += ", ";
                sql += sets[i];
            }
            sql += " WHERE id = ?";

            QSqlQuery q(db);
            q.prepare(QString::fromStdString(sql));
            for (size_t i = 0; i < binds.size(); ++i) {
                q.bindValue(static_cast<int>(i), binds[i]);
            }
            q.bindValue(static_cast<int>(binds.size()), id);

            if (!q.exec()) {
                return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
            }
            vms::core::PeopleCountTracker::getInstance().reloadFromDb();
            return ApiUtils::createResponse({{"updated", true}, {"id", id}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 400, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // POST /api/counter/lines/reload — manual cache refresh (ops escape hatch).
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/counter/lines/reload")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireCounterAdmin(app, req, origin)) return std::move(*err);
        vms::core::PeopleCountTracker::getInstance().reloadFromDb();
        return ApiUtils::createResponse({{"reloaded", true}}, 200, origin);
    });
}

} // namespace vms::api
