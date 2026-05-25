#include "../../include/api/analytics_controller.h"
#include "../../include/database/db_manager.h"
#include "../../include/database/traffic_repository.h"
#include "../../include/utils/api_utils.h"
#include "../../include/utils/logger.h"
#include "../../include/middleware/auth_middleware.h"
#include "../core/ai_event_processor.h"
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <crow.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vms {
namespace api {

using json = nlohmann::json;

void AnalyticsController::registerRoutes(vms::server::VmsApp& app) {
    LOG_INFO("Registering analytics routes...");

    // GET /api/analytics/summary/<int>
    CROW_ROUTE(app, "/api/analytics/summary/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        // Analytics summary is permission-gated, not merely authenticated.
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);

        try {
            vms::database::TrafficRepository repo;
            auto summary = repo.getSummary(id);
            
            json res;
            res["camera_id"] = summary.camera_id;
            res["total_today"] = summary.total_today;
            res["total_in"] = summary.total_in;
            res["total_out"] = summary.total_out;
            res["peak_hour"] = summary.peak_hour;
            res["peak_count"] = summary.peak_count;
            
            return ApiUtils::createResponse(res, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // GET /api/analytics/traffic/<int> (Timeseries data)
    CROW_ROUTE(app, "/api/analytics/traffic/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);

        try {
            vms::database::TrafficRepository repo;
            // Default to last 24 hours
            std::time_t to = std::time(nullptr);
            std::time_t from = to - (24 * 3600);
            
            auto counts = repo.getCounts(id, from, to);
            json counts_arr = json::array();
            for (const auto& c : counts) {
                counts_arr.push_back({
                    {"period_start", c.period_start},
                    {"count", c.count},
                    {"direction", c.direction},
                    {"type", c.vehicle_type}
                });
            }
            return ApiUtils::createResponse({{"counts", counts_arr}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // Rolling-window PPE compliance snapshot. Query param `window_minutes`
    // accepts 1..60 and defaults to 60. Access follows ANALYTICS_READ.
    CROW_ROUTE(app, "/api/analytics/ppe_compliance")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) {
            return std::move(*err);
        }

        int window_minutes = 60;
        try {
            std::string raw = req.url_params.get("window_minutes")
                ? std::string(req.url_params.get("window_minutes"))
                : std::string("");
            if (!raw.empty()) {
                window_minutes = std::atoi(raw.c_str());
            }
        } catch (...) {
            window_minutes = 60;
        }

        try {
            auto out = vms::core::PpeComplianceAggregator::getInstance().snapshot(window_minutes);
            return ApiUtils::createResponse(out, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────────
    // GET /api/analytics/ppe/status — operator-facing readiness probe.
    //
    // Combines two orthogonal signals so the dashboard can stop lying:
    //   1. Config (intent): how many cameras have ai_config.ppe=true, plus
    //      env-level VMS_AI_ENABLE_PPE / VMS_AI_DISABLE_PPE force-flags.
    //   2. Runtime (reality): when did we last receive a PPE telemetry
    //      tick / a violation? How many distinct cameras have ever
    //      ticked since process start?
    //
    // Health is derived from the gap between intent and reality:
    //   no_cameras → no camera configured, no env-forced enable → operator
    //                hasn't turned PPE on anywhere; "active" badge would lie.
    //   inactive   → configured/forced but the aggregator has never ticked
    //                → ai_worker likely failed to load the engine on those
    //                  cameras (check ai_worker log). Backend can't tell the
    //                  difference between "model load failed" and "scene was
    //                  empty since boot" — both look identical from here.
    //   stale      → ticked before but the last tick is older than the
    //                cutoff (5 min by default, env VMS_PPE_STATE_STALE_S).
    //                Either AI worker crashed silently or every PPE-enabled
    //                camera went dark simultaneously.
    //   ok         → configured + at least one camera has ticked + last tick
    //                is within the staleness cutoff.
    //
    // ANALYTICS_READ. Safe to poll at UI cadence (one short SELECT on
    // cameras + two atomic loads + one brief mutex acquisition).
    // ─────────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/analytics/ppe/status")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) {
            return std::move(*err);
        }

        try {
            // ── Config probe: SELECT id, ai_config from cameras and parse JSON
            //    in C++. Doing this with json_extract() in SQL would diverge
            //    SQLite vs Postgres syntax — repeating BUG-NIGHT-01-class
            //    portability bug. ──────────────────────────────────────────
            std::vector<int> enabled_cameras;
            int  total_cameras_with_config = 0;
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (db.isValid() && db.isOpen()) {
                QSqlQuery q(db);
                q.prepare("SELECT id, ai_config FROM cameras");
                if (q.exec()) {
                    while (q.next()) {
                        const int cam_id = q.value(0).toInt();
                        const std::string raw = q.value(1).isNull()
                            ? std::string{}
                            : q.value(1).toString().toStdString();
                        if (raw.empty()) continue;
                        ++total_cameras_with_config;
                        try {
                            auto cfg = json::parse(raw);
                            // ai_worker reads "ppe" key — same name in both
                            // cmdline JSON (line 581) and DB column (line 580).
                            if (cfg.is_object() && cfg.value("ppe", false)) {
                                enabled_cameras.push_back(cam_id);
                            }
                        } catch (...) {
                            // Malformed ai_config — count but don't crash.
                            // Same tolerance as PeopleCountTracker uses for
                            // counting_lines.object_classes_json.
                        }
                    }
                } else {
                    LOG_WARN("[ppe/status] cameras query failed: {}",
                             q.lastError().text().toStdString());
                }
            }

            // ── Env flags. Treat the same set of truthy strings as ai_worker
            //    main.cpp:env_truthy (1/true/yes/on, case-insensitive). ─────
            auto env_truthy = [](const char* k) -> bool {
                const char* v = std::getenv(k);
                if (!v || !*v) return false;
                std::string s = v;
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                return s == "1" || s == "true" || s == "yes" || s == "on";
            };
            const bool env_force_enable  = env_truthy("VMS_AI_ENABLE_PPE");
            const bool env_force_disable = env_truthy("VMS_AI_DISABLE_PPE");

            // ── Runtime telemetry from aggregator ───────────────────────────
            auto& agg = vms::core::PpeComplianceAggregator::getInstance();
            const int64_t last_tick_ms      = agg.lastTickMs();
            const int64_t last_violation_ms = agg.lastViolationMs();
            const std::size_t active_cams   = agg.cameraCount();
            // 60-min totals — reusing the existing snapshot path keeps one
            // source of truth for the compliance figures the dashboard
            // already shows next to the badge.
            const auto window_snapshot = agg.snapshot(60);

            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            // ── Staleness cutoff (env-tunable). Clamped to [30s, 1h] so a
            //    typo can't disable the freshness check entirely. ──────────
            int stale_after_s = 300;
            if (const char* e = std::getenv("VMS_PPE_STATE_STALE_S")) {
                if (*e) {
                    int v = std::atoi(e);
                    stale_after_s = std::clamp(v, 30, 3600);
                }
            }
            const int64_t stale_after_ms = static_cast<int64_t>(stale_after_s) * 1000;

            // ── Health derivation. The matrix below is mirrored byte-for-byte
            //    in tests/test_ppe_status.cpp — keep them in sync. ─────────
            //    intent_enabled = config or env says PPE should run somewhere.
            const bool intent_enabled = env_force_enable
                                     || (!env_force_disable && !enabled_cameras.empty());
            std::string health;
            if (!intent_enabled) {
                health = "no_cameras";
            } else if (last_tick_ms <= 0 || active_cams == 0) {
                health = "inactive";
            } else if ((now_ms - last_tick_ms) > stale_after_ms) {
                health = "stale";
            } else {
                health = "ok";
            }

            json enabled_arr = json::array();
            for (int id : enabled_cameras) enabled_arr.push_back(id);

            json out = {
                {"health", health},
                {"generated_at_ms", now_ms},
                {"config", {
                    {"enabled_cameras",   enabled_arr},
                    {"enabled_count",     static_cast<int>(enabled_cameras.size())},
                    {"total_cameras",     total_cameras_with_config},
                    {"env_force_enable",  env_force_enable},
                    {"env_force_disable", env_force_disable}
                }},
                {"runtime", {
                    {"active_cameras",            static_cast<int>(active_cams)},
                    {"last_tick_ms",              last_tick_ms},
                    {"last_violation_ms",         last_violation_ms},
                    {"seconds_since_last_tick",   last_tick_ms      > 0 ? (now_ms - last_tick_ms)      / 1000 : -1},
                    {"seconds_since_last_violation",
                                                  last_violation_ms > 0 ? (now_ms - last_violation_ms) / 1000 : -1},
                    {"stale_after_s",             stale_after_s},
                    {"compliance_60min",          window_snapshot.value("global", json::object())}
                }}
            };
            return ApiUtils::createResponse(out, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin, "ppe/status");
        }
    });
}

} // namespace api
} // namespace vms
