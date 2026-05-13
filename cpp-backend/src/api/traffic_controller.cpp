#include "api/traffic_controller.h"
#include "database/audit_repository.h"
#include "database/traffic_repository.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <ctime>

using json = nlohmann::json;

namespace vms {
namespace api {

// Helper: serialize TrafficCount → JSON
static json tcToJson(const TrafficCount& tc) {
    return {
        {"id",           tc.id},
        {"camera_id",    tc.camera_id},
        {"roi_id",       tc.roi_id},
        {"direction",    tc.direction},
        {"vehicle_type", tc.vehicle_type},
        {"count",        tc.count},
        {"period_start", tc.period_start},
        {"period_end",   tc.period_end},
        {"created_at",   tc.created_at}
    };
}

static json summaryToJson(const TrafficSummary& s) {
    return {
        {"camera_id",    s.camera_id},
        {"total_today",  s.total_today},
        {"total_in",     s.total_in},
        {"total_out",    s.total_out},
        {"peak_hour",    s.peak_hour},
        {"peak_count",   s.peak_count}
    };
}

void TrafficController::registerRoutes(vms::server::VmsApp& app) {

    // ----------------------------------------------------------------
    // GET /api/traffic/counts
    // Query params: camera_id (int), from (unix ts), to (unix ts), limit (int)
    // ----------------------------------------------------------------
    CROW_ROUTE(app, "/api/traffic/counts")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);

        try {
            int camera_id = -1;
            std::time_t from_ts = 0, to_ts = 0;
            int limit = 200;

            if (req.url_params.get("camera_id"))
                try { camera_id = std::stoi(req.url_params.get("camera_id")); }
                catch(...) { return ApiUtils::createErrorResponse("Invalid camera_id", 400, origin); }

            if (req.url_params.get("from"))
                try { from_ts = (std::time_t)std::stoll(req.url_params.get("from")); }
                catch(...) { LOG_DEBUG("TrafficController: Invalid 'from' param, using default"); }

            if (req.url_params.get("to"))
                try { to_ts = (std::time_t)std::stoll(req.url_params.get("to")); }
                catch(...) { LOG_DEBUG("TrafficController: Invalid 'to' param, using default"); }

            if (req.url_params.get("limit"))
                try {
                    limit = std::stoi(req.url_params.get("limit"));
                    if (limit < 1 || limit > 5000) limit = 200;
                } catch(...) { LOG_DEBUG("TrafficController: Invalid 'limit' param, using default"); }

            database::TrafficRepository repo;
            auto counts = repo.getCounts(camera_id, from_ts, to_ts, limit);

            json arr = json::array();
            for (const auto& tc : counts) arr.push_back(tcToJson(tc));

            return ApiUtils::createResponse({{"counts", arr}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ----------------------------------------------------------------
    // POST /api/traffic/counts
    // Body: { camera_id, roi_id?, direction, vehicle_type, count,
    //         period_start, period_end }
    // Admin-only: this is a write into the analytics table that bypasses
    // the AI pipeline. Zero existing callers (neither frontend nor any
    // internal C++ writer); kept for operator manual-correction scenarios.
    // ----------------------------------------------------------------
    CROW_ROUTE(app, "/api/traffic/counts")
    .methods(crow::HTTPMethod::Post)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requireAdmin(ctx, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);

            if (!body.contains("camera_id") || !body.contains("count") ||
                !body.contains("period_start") || !body.contains("period_end")) {
                return ApiUtils::createErrorResponse(
                    "Missing required fields: camera_id, count, period_start, period_end",
                    400, origin);
            }

            TrafficCount tc;
            tc.camera_id    = body.value("camera_id",    -1);
            tc.roi_id       = body.value("roi_id",       -1);
            tc.direction    = body.value("direction",    "both");
            tc.vehicle_type = body.value("vehicle_type", "all");
            tc.count        = body.value("count",        0);
            tc.period_start = (std::time_t)body.value("period_start", (long long)0);
            tc.period_end   = (std::time_t)body.value("period_end",   (long long)0);

            // Validate
            if (tc.camera_id < 0)
                return ApiUtils::createErrorResponse("Invalid camera_id", 400, origin);
            if (tc.count < 0 || tc.count > 100000)
                return ApiUtils::createErrorResponse("count out of range (0-100000)", 400, origin);
            if (tc.period_start <= 0 || tc.period_end <= 0 || tc.period_end < tc.period_start)
                return ApiUtils::createErrorResponse("Invalid time period", 400, origin);

            database::TrafficRepository repo;
            if (repo.insertCount(tc)) {
                // Audit: manual write into analytics — log who, target camera, magnitude.
                if (ctx.user.has_value()) {
                    try {
                        vms::database::AuditRepository audit;
                        audit.insertLog(ctx.user->id, "TRAFFIC_COUNT_INSERT",
                                        "camera_id=" + std::to_string(tc.camera_id)
                                        + " count=" + std::to_string(tc.count)
                                        + " vehicle=" + tc.vehicle_type);
                    } catch (...) { /* never break on audit failure */ }
                }
                return ApiUtils::createResponse(json::object(), 201, origin);
            }

            return ApiUtils::createErrorResponse("Failed to insert traffic count", 500, origin);
        } catch (const json::exception& e) {
            return ApiUtils::createErrorResponse(
                std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ----------------------------------------------------------------
    // GET /api/traffic/summary?camera_id=<id>
    // ----------------------------------------------------------------
    CROW_ROUTE(app, "/api/traffic/summary")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);

        try {
            int camera_id = -1;
            if (req.url_params.get("camera_id"))
                try { camera_id = std::stoi(req.url_params.get("camera_id")); }
                catch(...) { return ApiUtils::createErrorResponse("Invalid camera_id", 400, origin); }

            if (camera_id < 0)
                return ApiUtils::createErrorResponse("camera_id is required", 400, origin);

            database::TrafficRepository repo;
            auto summary = repo.getSummary(camera_id);

            return ApiUtils::createResponse(summaryToJson(summary), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });
}

} // namespace api
} // namespace vms
