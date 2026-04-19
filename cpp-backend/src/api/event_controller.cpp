#include "api/event_controller.h"
#include "core/event_manager.h"
#include "utils/api_utils.h"
#include "utils/camera_name_cache.h"
#include "utils/media_signer.h"
#include "database/db_manager.h"
#include "database/json_serialization.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <optional>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

using json = nlohmann::json;

namespace vms {
namespace api {

namespace {

std::string normalizeEventType(const std::string& raw_type) {
    std::string lower = raw_type;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lower == "ppe_violation" || lower == "ppe") {
        return "ppe";
    }
    if (raw_type == "FACE_RECOGNIZED" || lower == "face_recognition" || lower == "face") {
        return "face";
    }
    if (lower.find("_detected") != std::string::npos || lower == "detection") {
        return "detection";
    }
    return raw_type;
}

long long normalizeQueryTimestamp(long long timestamp) {
    // Accept both milliseconds and seconds in query params; DB stores seconds.
    return timestamp > 9999999999LL ? (timestamp / 1000) : timestamp;
}

void bindNormalizedEventType(QSqlQuery& query, int& bind_idx, const std::string& normalized_type) {
    if (normalized_type == "ppe") {
        query.bindValue(bind_idx++, QStringLiteral("ppe"));
        query.bindValue(bind_idx++, QStringLiteral("ppe_violation"));
    } else if (normalized_type == "face") {
        query.bindValue(bind_idx++, QStringLiteral("face"));
        query.bindValue(bind_idx++, QStringLiteral("face_recognition"));
        query.bindValue(bind_idx++, QStringLiteral("FACE_RECOGNIZED"));
    } else if (normalized_type == "detection") {
        query.bindValue(bind_idx++, QStringLiteral("detection"));
        query.bindValue(bind_idx++, QStringLiteral("%_detected"));
    } else if (!normalized_type.empty()) {
        query.bindValue(bind_idx++, QString::fromStdString(normalized_type));
    }
}

int getFilteredEventCount(int camera_id,
                          const std::string& normalized_type,
                          std::optional<long long> start_time,
                          std::optional<long long> end_time) {
    auto& db = vms::database::DbManager::getInstance();
    QSqlDatabase conn = db.getThreadConnection();
    if (!conn.isOpen()) {
        return 0;
    }

    std::string sql = "SELECT COUNT(*) FROM events";
    std::vector<std::string> where_clauses;

    if (camera_id >= 0) {
        where_clauses.emplace_back("camera_id = ?");
    }
    if (!normalized_type.empty()) {
        if (normalized_type == "ppe") {
            where_clauses.emplace_back("(event_type = ? OR event_type = ?)");
        } else if (normalized_type == "face") {
            where_clauses.emplace_back("(event_type = ? OR event_type = ? OR event_type = ?)");
        } else if (normalized_type == "detection") {
            where_clauses.emplace_back("(event_type = ? OR event_type LIKE ?)");
        } else {
            where_clauses.emplace_back("event_type = ?");
        }
    }
    if (start_time.has_value()) {
        where_clauses.emplace_back("timestamp >= ?");
    }
    if (end_time.has_value()) {
        where_clauses.emplace_back("timestamp <= ?");
    }

    if (!where_clauses.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < where_clauses.size(); ++i) {
            if (i > 0) sql += " AND ";
            sql += where_clauses[i];
        }
    }

    QSqlQuery query(conn);
    query.prepare(QString::fromStdString(sql));

    int bind_idx = 0;
    if (camera_id >= 0) {
        query.bindValue(bind_idx++, camera_id);
    }
    bindNormalizedEventType(query, bind_idx, normalized_type);
    if (start_time.has_value()) {
        query.bindValue(bind_idx++, static_cast<qlonglong>(start_time.value()));
    }
    if (end_time.has_value()) {
        query.bindValue(bind_idx++, static_cast<qlonglong>(end_time.value()));
    }

    if (!query.exec() || !query.next()) {
        return 0;
    }

    return query.value(0).toInt();
}

crow::response createEventsListResponse(const json& events,
                                        int total,
                                        int page,
                                        int limit,
                                        const std::string& origin) {
    crow::response res;
    res.code = 200;
    res.set_header("Content-Type", "application/json");
    ApiUtils::applyCors(res, origin);
    res.body = json{
        {"success", true},
        {"data", {
            {"events", events},
            {"total", total},
            {"page", page},
            {"limit", limit}
        }},
        {"error", nullptr}
    }.dump();
    return res;
}

} // namespace

// Helper to enrich event with base64 snapshot and fix timestamps
static json enrichEvent(const Event& evt) {
    json j = evt;
    const std::string normalized_type = normalizeEventType(evt.event_type);

    // 1. Fix Timestamp (seconds -> milliseconds for JS)
    j["timestamp"] = (uint64_t)evt.timestamp * 1000;
    j["event_type"] = normalized_type;
    j["raw_event_type"] = evt.event_type;

    // 2. Adjust Event Type for frontend compatibility
    // Frontend filters for 'detection', 'motion', 'alert', 'system'
    if (normalized_type == "detection") {
        j["title"] = evt.description;
        j["label"] = evt.event_type; // Use the raw event type as label
        j["message"] = evt.description;
    } else {
        j["label"] = normalized_type;
        j["message"] = evt.description;
    }

    // 2.5. Face Recognition Logic (Người quen / Người lạ)
    if (normalized_type == "face") {
        std::string person_name = "Người lạ";
        bool known = false;
        if (!evt.metadata_json.empty()) {
            try {
                json meta = json::parse(evt.metadata_json);
                if (meta.contains("name")) {
                    std::string name = meta["name"].get<std::string>();
                    if (!name.empty() && name != "unknown" && name != "Unknown") {
                        person_name = "Người quen: " + name;
                        known = true;
                    }
                }
            } catch(...) {
                // Ignore parse errors
            }
        }
        j["title"] = person_name;
        j["label"] = known ? "Người quen" : "Người lạ";
        j["message"] = evt.description;
    }

    // 3. Add Static URL for Snapshot instead of Base64 encoding
    // This drastically reduces JSON payload size and CPU usage.
    if (!evt.snapshot_path.empty()) {
        try {
            std::filesystem::path p(evt.snapshot_path);
            std::string filename = p.filename().string();
            const auto& media_cfg = vms::Config::getInstance().getMediaSigningConfig();
            vms::utils::MediaAccessScope media_scope;
            media_scope.scope = "snapshot";
            media_scope.camera_id = evt.camera_id;
            media_scope.resource_id = evt.id;
            j["snapshot_url"] = vms::utils::presignPath(
                "/api/snapshots/files/" + filename,
                media_cfg.snapshot_ttl_seconds,
                media_scope
            );
        } catch (const std::exception& e) {
            j["snapshot_error"] = e.what();
        }
    }

    return j;
}

void EventController::registerRoutes(vms::server::VmsApp& app) {
    // GET events with filters
    CROW_ROUTE(app, "/api/events")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            auto& event_mgr = vms::core::EventManager::getInstance();

            int camera_id = -1;
            int limit = 100;
            int offset = 0;
            std::string event_type_filter = "";
            std::optional<long long> start_time;
            std::optional<long long> end_time;

            try {
                auto camera_id_str = req.url_params.get("camera_id");
                if (camera_id_str) camera_id = std::stoi(camera_id_str);

                auto limit_str = req.url_params.get("limit");
                if (limit_str) limit = std::stoi(limit_str);
                if (limit < 1 || limit > 10000) limit = 100;

                auto offset_str = req.url_params.get("offset");
                if (offset_str) offset = std::stoi(offset_str);
                if (offset < 0) offset = 0;

                auto event_type_str = req.url_params.get("event_type");
                if (event_type_str) event_type_filter = normalizeEventType(std::string(event_type_str));

                auto start_time_str = req.url_params.get("start_time");
                if (start_time_str) start_time = normalizeQueryTimestamp(std::stoll(start_time_str));

                auto end_time_str = req.url_params.get("end_time");
                if (end_time_str) end_time = normalizeQueryTimestamp(std::stoll(end_time_str));
            } catch (const std::exception&) {
                return ApiUtils::createErrorResponse(
                    "Invalid query parameter: camera_id, limit, offset, start_time, and end_time must be integers",
                    400,
                    origin
                );
            }

            auto events = event_mgr.getEvents(camera_id, limit, offset, event_type_filter, start_time, end_time);

            auto& name_cache = vms::utils::CameraNameCache::getInstance();

            json event_list = json::array();
            for (const auto& evt : events) {
                json j = enrichEvent(evt);
                j["camera_name"] = name_cache.getName(evt.camera_id);
                event_list.push_back(j);
            }

            int total = getFilteredEventCount(camera_id, event_type_filter, start_time, end_time);
            int page = (limit > 0) ? (offset / limit) + 1 : 1;

            return createEventsListResponse(event_list, total, page, limit, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET event stats
    CROW_ROUTE(app, "/api/events/stats")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            auto& event_mgr = vms::core::EventManager::getInstance();

            int camera_id = -1;
            auto camera_id_str = req.url_params.get("camera_id");
            if (camera_id_str) camera_id = std::stoi(camera_id_str);

            int count = event_mgr.getEventCount(camera_id);
            return ApiUtils::createResponse({{"total_events", count}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET event analytics
    CROW_ROUTE(app, "/api/events/analytics")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        try {
            auto& db = vms::database::DbManager::getInstance();
            QSqlDatabase conn = db.getThreadConnection();

            std::string sql;
            if (vms::Config::getInstance().getDatabaseConfig().driver == "postgresql") {
                sql = R"(
                    SELECT event_type, to_char(to_timestamp(timestamp), 'YYYY-MM-DD') as day, COUNT(*) as count
                    FROM events
                    WHERE timestamp >= EXTRACT(EPOCH FROM (NOW() - INTERVAL '7 days'))
                    GROUP BY event_type, day
                    ORDER BY day ASC
                )";
            } else {
                sql = R"(
                    SELECT event_type, date(timestamp, 'unixepoch', 'localtime') as day, COUNT(*) as count
                    FROM events
                    WHERE timestamp >= strftime('%s', 'now', '-7 days')
                    GROUP BY event_type, date(timestamp, 'unixepoch', 'localtime')
                    ORDER BY day ASC
                )";
            }
            json result = json::array();
            QSqlQuery query(conn);
            query.prepare(QString::fromStdString(sql));
            if (query.exec()) {
                while (query.next()) {
                    std::string type = query.value(0).isNull() ? "unknown" : query.value(0).toString().toStdString();
                    std::string day = query.value(1).isNull() ? "N/A" : query.value(1).toString().toStdString();
                    int count = query.value(2).toInt();
                    result.push_back({{"type", type}, {"day", day}, {"count", count}});
                }
            }
            return ApiUtils::createResponse({{"analytics", result}}, 200, origin);
        } catch (const std::exception& e) {
             return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET event timeline
    CROW_ROUTE(app, "/api/events/timeline")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
             auto& event_mgr = vms::core::EventManager::getInstance();
             int camera_id = -1;
             try {
                 auto camera_id_str = req.url_params.get("camera_id");
                 if (camera_id_str) camera_id = std::stoi(camera_id_str);
             } catch (const std::exception&) {
                 return ApiUtils::createErrorResponse("Invalid camera_id parameter", 400, origin);
             }

             auto events = event_mgr.getEvents(camera_id, 1000, 0);

             json timeline = json::array();
             for(const auto& evt : events) {
                 timeline.push_back({
                     {"id", evt.id},
                     {"timestamp", evt.timestamp},
                     {"type", evt.event_type}
                 });
             }

             return ApiUtils::createResponse({{"events", timeline}}, 200, origin);
        } catch (const std::exception& e) {
             return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET attendance
    CROW_ROUTE(app, "/api/attendance")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            std::string date_str = "";
            if (req.url_params.get("date")) {
                date_str = req.url_params.get("date");
            }
            if (date_str.empty()) {
                // Default to today YYYY-MM-DD
                auto t = std::time(nullptr);
                auto tm = *std::localtime(&t);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
                date_str = buf;
            }

            auto& db = vms::database::DbManager::getInstance();
            QSqlDatabase conn = db.getThreadConnection();

            // Extract the name from metadata_json, group by name, and find min/max timestamp for the specific date.
            std::string sql;
            if (vms::Config::getInstance().getDatabaseConfig().driver == "postgresql") {
                sql = R"(
                    SELECT
                        metadata_json->>'name' as person_name,
                        MIN(timestamp) as check_in,
                        MAX(timestamp) as check_out
                    FROM events
                    WHERE (event_type = 'face' OR event_type = 'face_recognition')
                      AND metadata_json->>'name' IS NOT NULL
                      AND metadata_json->>'name' != 'unknown'
                      AND metadata_json->>'name' != 'Unknown'
                      AND to_char(to_timestamp(timestamp), 'YYYY-MM-DD') = ?
                    GROUP BY person_name
                )";
            } else {
                sql = R"(
                    SELECT
                        json_extract(metadata_json, '$.name') as person_name,
                        MIN(timestamp) as check_in,
                        MAX(timestamp) as check_out
                    FROM events
                    WHERE (event_type = 'face' OR event_type = 'face_recognition')
                      AND json_extract(metadata_json, '$.name') IS NOT NULL
                      AND json_extract(metadata_json, '$.name') != 'unknown'
                      AND json_extract(metadata_json, '$.name') != 'Unknown'
                      AND date(timestamp, 'unixepoch', 'localtime') = ?
                    GROUP BY person_name
                )";
            }

            json result = json::array();
            QSqlQuery query(conn);
            query.prepare(QString::fromStdString(sql));
            query.bindValue(0, QString::fromStdString(date_str));

            if (query.exec()) {
                while (query.next()) {
                    std::string name = query.value(0).isNull() ? "Người Lạ" : query.value(0).toString().toStdString();
                    uint64_t check_in = query.value(1).toLongLong() * 1000; // to ms
                    uint64_t check_out = query.value(2).toLongLong() * 1000;

                    result.push_back({
                        {"person_name", name},
                        {"check_in", check_in},
                        {"check_out", check_out},
                        {"date", date_str}
                    });
                }
            } else {
                return ApiUtils::createErrorResponse("Database query failed", 500, origin);
            }

            return ApiUtils::createResponse({{"attendance", result}}, 200, origin);
        } catch (const std::exception& e) {
             return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET event video clip
    CROW_ROUTE(app, "/api/events/<string>/video")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req, std::string id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            auto& event_mgr = vms::core::EventManager::getInstance();
            auto event_opt = event_mgr.getEvent(id);

            if (!event_opt) {
                return ApiUtils::createErrorResponse("Event not found", 404, origin);
            }

            auto evt = event_opt.value();

            // Extract video_path from metadata JSON
            std::string video_path;
            try {
                if (!evt.metadata_json.empty()) {
                    json meta = json::parse(evt.metadata_json);
                    if (meta.contains("video_path")) {
                        video_path = meta["video_path"].get<std::string>();
                    }
                }
            } catch (...) {
                // Ignore JSON parse errors
            }

            if (video_path.empty() || !std::filesystem::exists(video_path)) {
                return ApiUtils::createErrorResponse("Video not found", 404, origin);
            }

            // FIX #10: Use set_static_file_info instead of loading entire video into RAM.
            auto file_size = std::filesystem::file_size(video_path);
            constexpr uintmax_t MAX_VIDEO_SIZE = 500 * 1024 * 1024; // 500MB limit
            if (file_size > MAX_VIDEO_SIZE) {
                return ApiUtils::createErrorResponse("Video file too large", 413, origin);
            }

            crow::response res;
            res.set_static_file_info(video_path);
            res.set_header("Content-Type", "video/mp4");
            std::string allowed_origin = origin.empty() ? "*" : origin;
            res.set_header("Access-Control-Allow-Origin", allowed_origin);
            return res;

        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // /api/events/<string> (GET, DELETE)
    CROW_ROUTE(app, "/api/events/<string>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([](const crow::request& req, std::string id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& event_mgr = vms::core::EventManager::getInstance();

        // GET event by ID
        if (req.method == crow::HTTPMethod::Get) {
            try {
                auto event_opt = event_mgr.getEvent(id);
                if (event_opt) {
                    json response = enrichEvent(event_opt.value());
                    return ApiUtils::createResponse(response, 200, origin);
                }
                return ApiUtils::createErrorResponse("Event not found", 404, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        // DELETE event
        if (req.method == crow::HTTPMethod::Delete) {
            try {
                if (event_mgr.deleteEvent(id)) {
                    return ApiUtils::createResponse(json::object(), 200, origin);
                }
                return ApiUtils::createErrorResponse("Event not found or failed to delete", 404, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });
}

} // namespace api
} // namespace vms
