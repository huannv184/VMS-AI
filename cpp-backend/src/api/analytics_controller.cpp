#include "../../include/api/analytics_controller.h"
#include "../../include/database/traffic_repository.h"
#include "../../include/utils/api_utils.h"
#include "../../include/utils/logger.h"
#include <crow.h>
#include <nlohmann/json.hpp>

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
}

} // namespace api
} // namespace vms
