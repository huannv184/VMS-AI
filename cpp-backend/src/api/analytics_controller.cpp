#include "../../include/api/analytics_controller.h"
#include "../../include/database/traffic_repository.h"
#include "../../include/utils/api_utils.h"
#include "../../include/utils/logger.h"
#include "../../include/middleware/auth_middleware.h"
#include "../core/ai_event_processor.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <string>

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
        
        // Auth check (Enterprise)
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);

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

    // 2026-05-20 PPE compliance rolling-window stats. Pre-fix the
    // dashboard computed compliance as "100 - violations*2" — a
    // placeholder that drifted with absolute violation count and
    // ignored how many compliant persons were observed (no denominator).
    // ai_worker now emits per-frame (compliant, violating) PPE-person
    // counts; PpeComplianceAggregator rolls a 60-minute window per
    // camera; this endpoint serializes the snapshot.
    //
    // Query params:
    //   window_minutes — 1..60, default 60.
    //
    // Returns:
    //   { window_minutes, generated_at_ms,
    //     global:    { compliant_ticks, violating_ticks, compliance_rate },
    //     per_camera:{ "<id>": {...}, ... } }
    //
    // RBAC: ANALYTICS_READ (same scope as the rest of analytics surface;
    // anpr_controller / attendance_controller follow this pattern).
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
}

} // namespace api
} // namespace vms
