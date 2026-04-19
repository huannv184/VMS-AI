#include "api/site_controller.h"
#include "database/site_repository.h"
#include "utils/api_utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace vms {
namespace api {

void SiteController::registerRoutes(vms::server::VmsApp& app) {
    
    // GET /api/sites
    CROW_ROUTE(app, "/api/sites")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto sites = database::SiteRepository::getAllSites();
        json j = json::array();
        for (const auto& s : sites) {
            j.push_back({
                {"id", s.id},
                {"name", s.name},
                {"address", s.address},
                {"parent_site_id", s.parent_site_id},
                {"description", s.description},
                {"created_at", s.created_at}
            });
        }
        return ApiUtils::createResponse({{"sites", j}}, 200, origin);
    });

    // POST /api/sites
    CROW_ROUTE(app, "/api/sites")
    .methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
            auto body = json::parse(req.body);
            vms::Site s;
            s.name = body.value("name", "");
            s.address = body.value("address", "");
            s.parent_site_id = body.value("parent_site_id", -1);
            s.description = body.value("description", "");
            
            if (database::SiteRepository::createSite(s)) {
                return ApiUtils::createResponse({{"id", s.id}}, 201, origin);
            }
        } catch (const nlohmann::json::exception& e) {
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
        return ApiUtils::createErrorResponse("Failed to create site", 500, origin);
    });

    // DELETE /api/sites/<int>
    CROW_ROUTE(app, "/api/sites/<int>")
    .methods(crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        if (database::SiteRepository::deleteSite(id)) {
            return ApiUtils::createResponse(json::object(), 200, origin);
        }
        return ApiUtils::createErrorResponse("Failed to delete site", 500, origin);
    });
}

} // namespace api
} // namespace vms
