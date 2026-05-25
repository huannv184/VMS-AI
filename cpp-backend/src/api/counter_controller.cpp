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
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/counter_bucket_aggregator.h"
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

// Compute server-local midnight epoch — used as the default `from` when the
// caller omits a time range. Matches TrafficRepository::getSummary semantics
// so operators get the same "today" boundary as the legacy traffic dashboard.
int64_t todayMidnightEpoch() {
    std::time_t now = std::time(nullptr);
    std::tm     lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0;
    return static_cast<int64_t>(std::mktime(&lt));
}

// Convert an epoch second to the local hour-of-day (0..23). Used for the
// peak-hour roll-up in /summary so the FE can render "Giờ cao điểm: 14h".
int hourOfDayLocal(int64_t epoch_s) {
    std::time_t t = static_cast<std::time_t>(epoch_s);
    std::tm     lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return lt.tm_hour;
}

// Parse a required integer query parameter. Sets *out and returns nullopt on
// success; otherwise returns a 400 response. Mirrors the validation pattern
// used by getCountingLines above for consistency with the rest of the
// controller.
std::optional<crow::response> parseIntQuery(const crow::request& req,
                                            const char* key,
                                            int* out,
                                            const std::string& origin,
                                            bool required = true) {
    const char* raw = req.url_params.get(key);
    if (!raw || !*raw) {
        if (!required) return std::nullopt;
        return ApiUtils::createErrorResponse(
            std::string{"Missing required query parameter: "} + key, 400, origin);
    }
    try {
        *out = std::stoi(raw);
    } catch (const std::exception&) {
        return ApiUtils::createErrorResponse(
            std::string{"Invalid query parameter: "} + key + " must be an integer",
            400, origin);
    }
    return std::nullopt;
}

std::optional<crow::response> parseInt64Query(const crow::request& req,
                                              const char* key,
                                              int64_t* out,
                                              const std::string& origin) {
    const char* raw = req.url_params.get(key);
    if (!raw || !*raw) return std::nullopt;  // optional → leave default
    try {
        *out = static_cast<int64_t>(std::stoll(raw));
    } catch (const std::exception&) {
        return ApiUtils::createErrorResponse(
            std::string{"Invalid query parameter: "} + key + " must be an integer",
            400, origin);
    }
    return std::nullopt;
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
                    { LOG_ERROR("[DB] query failed in counter_controller: {}", q.lastError().text().toStdString()); return ApiUtils::createErrorResponse("Internal database error", 500, origin); }
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
                { LOG_ERROR("[DB] query failed in counter_controller: {}", q.lastError().text().toStdString()); return ApiUtils::createErrorResponse("Internal database error", 500, origin); }
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
                    { LOG_ERROR("[DB] query failed in counter_controller: {}", q.lastError().text().toStdString()); return ApiUtils::createErrorResponse("Internal database error", 500, origin); }
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
                { LOG_ERROR("[DB] query failed in counter_controller: {}", q.lastError().text().toStdString()); return ApiUtils::createErrorResponse("Internal database error", 500, origin); }
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

    // ─────────────────────────────────────────────────────────────────────
    // GET /api/counter/summary?camera_id=<id>&from=<unix>&to=<unix>
    //   Returns aggregated in/out counts from `counter_buckets_1m` for the
    //   requested camera + window. Defaults to today (local midnight → now).
    //   Source of truth: bucket aggregator. This is the SAME data the live
    //   pipeline writes — unlike the legacy /api/analytics/summary route
    //   which queries the orthogonal `traffic_counts` table.
    //
    //   Why a new endpoint instead of swapping the legacy one: traffic_counts
    //   is still alive (vehicle counting + ANPR). Splitting the routes keeps
    //   semantics explicit so a future operator does not conflate the two
    //   data sources again.
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/counter/summary")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireCounterRead(app, req, origin)) return std::move(*err);

        int camera_id = 0;
        if (auto err = parseIntQuery(req, "camera_id", &camera_id, origin)) return std::move(*err);

        const int64_t default_from = todayMidnightEpoch();
        const int64_t default_to   = static_cast<int64_t>(std::time(nullptr));
        int64_t from_ts = default_from;
        int64_t to_ts   = default_to;
        if (auto err = parseInt64Query(req, "from", &from_ts, origin)) return std::move(*err);
        if (auto err = parseInt64Query(req, "to",   &to_ts,   origin)) return std::move(*err);
        if (to_ts <= from_ts) {
            return ApiUtils::createErrorResponse("`to` must be greater than `from`", 400, origin);
        }
        // Cap range at 31 days — keeps result row count bounded (~44k buckets
        // max per camera) and prevents an accidental ?from=0 from scanning
        // the entire table.
        constexpr int64_t kMaxRangeSeconds = 31LL * 24 * 3600;
        if ((to_ts - from_ts) > kMaxRangeSeconds) {
            return ApiUtils::createErrorResponse(
                "range too large (max 31 days)", 400, origin);
        }

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            // Fetch buckets in the window — we'll aggregate in C++ so the
            // same code path produces both the per-line breakdown AND the
            // peak-hour roll-up without two round-trips.
            QSqlQuery q(db);
            q.prepare("SELECT source_id, ts_minute, in_count, out_count "
                      "FROM counter_buckets_1m "
                      "WHERE camera_id = ? AND source_kind = 'line' "
                      "  AND ts_minute >= ? AND ts_minute < ?");
            q.bindValue(0, camera_id);
            q.bindValue(1, static_cast<qlonglong>(from_ts));
            q.bindValue(2, static_cast<qlonglong>(to_ts));
            if (!q.exec()) {
                LOG_ERROR("[DB] counter/summary query failed: {}",
                          q.lastError().text().toStdString());
                return ApiUtils::createErrorResponse("Internal database error", 500, origin);
            }

            int total_in = 0, total_out = 0;
            std::unordered_map<int, std::pair<int,int>> per_line;       // line_id → (in, out)
            std::unordered_map<int, int>                per_hour_total; // hour → total

            while (q.next()) {
                const int     line_id   = q.value(0).toInt();
                const int64_t ts_minute = q.value(1).toLongLong();
                const int     in_c      = q.value(2).toInt();
                const int     out_c     = q.value(3).toInt();

                total_in  += in_c;
                total_out += out_c;
                auto& p = per_line[line_id];
                p.first  += in_c;
                p.second += out_c;

                per_hour_total[hourOfDayLocal(ts_minute)] += (in_c + out_c);
            }

            // Resolve line_id → line name (one extra query, small N).
            std::unordered_map<int, std::string> line_names;
            if (!per_line.empty()) {
                QSqlQuery nq(db);
                nq.prepare("SELECT id, name FROM counting_lines WHERE camera_id = ?");
                nq.bindValue(0, camera_id);
                if (nq.exec()) {
                    while (nq.next()) {
                        line_names[nq.value(0).toInt()] = nq.value(1).toString().toStdString();
                    }
                }
            }

            // Peak hour: the hour bucket with the largest total. Empty data
            // returns peak_hour = -1 (FE treats < 0 as "no data").
            int peak_hour = -1, peak_count = 0;
            for (const auto& [h, total] : per_hour_total) {
                if (total > peak_count) { peak_count = total; peak_hour = h; }
            }

            json lines_arr = json::array();
            for (const auto& [line_id, counts] : per_line) {
                auto it = line_names.find(line_id);
                lines_arr.push_back({
                    {"line_id",   line_id},
                    {"line_name", it != line_names.end() ? it->second : std::string{}},
                    {"in_count",  counts.first},
                    {"out_count", counts.second}
                });
            }

            return ApiUtils::createResponse({
                {"camera_id",   camera_id},
                {"from",        from_ts},
                {"to",          to_ts},
                {"total_in",    total_in},
                {"total_out",   total_out},
                {"total_today", total_in + total_out},
                {"peak_hour",   peak_hour},
                {"peak_count",  peak_count},
                {"lines",       lines_arr}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin, "counter/summary");
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // GET /api/counter/history?camera_id=<id>&from=<unix>&to=<unix>
    //   Returns the raw 1-minute bucket rows in the window. The FE uses this
    //   for the hourly timeline chart; no server-side bucketing beyond what
    //   the aggregator already did (UI is free to re-bin to 5m/1h/etc).
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/counter/history")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireCounterRead(app, req, origin)) return std::move(*err);

        int camera_id = 0;
        if (auto err = parseIntQuery(req, "camera_id", &camera_id, origin)) return std::move(*err);

        int64_t from_ts = todayMidnightEpoch();
        int64_t to_ts   = static_cast<int64_t>(std::time(nullptr));
        if (auto err = parseInt64Query(req, "from", &from_ts, origin)) return std::move(*err);
        if (auto err = parseInt64Query(req, "to",   &to_ts,   origin)) return std::move(*err);
        if (to_ts <= from_ts) {
            return ApiUtils::createErrorResponse("`to` must be greater than `from`", 400, origin);
        }
        constexpr int64_t kMaxRangeSeconds = 31LL * 24 * 3600;
        if ((to_ts - from_ts) > kMaxRangeSeconds) {
            return ApiUtils::createErrorResponse(
                "range too large (max 31 days)", 400, origin);
        }

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            QSqlQuery q(db);
            q.prepare("SELECT ts_minute, source_id, in_count, out_count "
                      "FROM counter_buckets_1m "
                      "WHERE camera_id = ? AND source_kind = 'line' "
                      "  AND ts_minute >= ? AND ts_minute < ? "
                      "ORDER BY ts_minute ASC, source_id ASC");
            q.bindValue(0, camera_id);
            q.bindValue(1, static_cast<qlonglong>(from_ts));
            q.bindValue(2, static_cast<qlonglong>(to_ts));
            if (!q.exec()) {
                LOG_ERROR("[DB] counter/history query failed: {}",
                          q.lastError().text().toStdString());
                return ApiUtils::createErrorResponse("Internal database error", 500, origin);
            }

            json points = json::array();
            while (q.next()) {
                const int64_t ts_minute = q.value(0).toLongLong();
                points.push_back({
                    {"ts_minute", ts_minute},
                    {"hour",      hourOfDayLocal(ts_minute)},  // convenience for legacy hourly chart
                    {"line_id",   q.value(1).toInt()},
                    {"in",        q.value(2).toInt()},
                    {"out",       q.value(3).toInt()},
                    {"count",     q.value(2).toInt() + q.value(3).toInt()}
                });
            }

            return ApiUtils::createResponse({
                {"camera_id", camera_id},
                {"from",      from_ts},
                {"to",        to_ts},
                {"points",    points}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin, "counter/history");
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // GET /api/counter/status — operator-facing health probe.
    //   Closes the "did the aggregator even start" question without making
    //   operators tail the backend log. Exposes:
    //     aggregator: running, sweep counters, last sweep duration
    //     tracker:    cache loaded, line count
    //
    //   Cheap to call (atomic loads + a single shared_lock); safe at any
    //   poll rate the UI feels like.
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/counter/status")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireCounterRead(app, req, origin)) return std::move(*err);

        const auto& agg = vms::core::CounterBucketAggregator::getInstance();
        auto&       trk = vms::core::PeopleCountTracker::getInstance();
        return ApiUtils::createResponse({
            {"aggregator", {
                {"running",          agg.running()},
                {"total_sweeps",     agg.totalSweeps()},
                {"total_upserted",   agg.totalUpserted()},
                {"last_sweep_ms",    agg.lastSweepMs()},
                {"interval_seconds", agg.intervalSeconds()},
                {"lookback_minutes", agg.lookbackMinutes()}
            }},
            {"tracker", {
                {"cache_loaded", trk.cacheLoaded()},
                {"total_lines",  trk.totalLineCount()}
            }}
        }, 200, origin);
    });
}

} // namespace vms::api
