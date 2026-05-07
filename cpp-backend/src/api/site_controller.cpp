#include "api/site_controller.h"
#include "database/audit_repository.h"
#include "database/site_repository.h"
#include "middleware/auth_middleware.h"
#include "server/vms_app.h"
#include "utils/api_utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace vms {
namespace api {

void SiteController::registerRoutes(vms::server::VmsApp& app) {

    // GET /api/sites — requires SITE_READ
    CROW_ROUTE(app, "/api/sites")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::SITE_READ, origin)) return std::move(*err);

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

    // POST /api/sites — requires SITE_WRITE
    CROW_ROUTE(app, "/api/sites")
    .methods(crow::HTTPMethod::Post)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::SITE_WRITE, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            vms::Site s;
            s.name = body.value("name", "");
            s.address = body.value("address", "");
            s.parent_site_id = body.value("parent_site_id", -1);
            s.description = body.value("description", "");

            if (s.name.empty())
                return ApiUtils::createErrorResponse("name is required", 400, origin);

            if (database::SiteRepository::createSite(s)) {
                database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "CREATE_SITE",
                                "Created site: " + s.name + " (id=" + std::to_string(s.id) + ")");
                return ApiUtils::createResponse({{"id", s.id}}, 201, origin);
            }
        } catch (const nlohmann::json::exception& e) {
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
        return ApiUtils::createErrorResponse("Failed to create site", 500, origin);
    });

    // PUT + DELETE /api/sites/<int> — both require SITE_WRITE
    CROW_ROUTE(app, "/api/sites/<int>")
    .methods(crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::SITE_WRITE, origin)) return std::move(*err);

        if (req.method == crow::HTTPMethod::Delete) {
            if (database::SiteRepository::deleteSite(id)) {
                database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "DELETE_SITE",
                                "Deleted site id=" + std::to_string(id));
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
            return ApiUtils::createErrorResponse("Failed to delete site", 500, origin);
        }

        // PUT
        try {
            auto body = json::parse(req.body);
            vms::Site s;
            s.id = id;
            s.name = body.value("name", "");
            s.address = body.value("address", "");
            s.parent_site_id = body.value("parent_site_id", -1);
            s.description = body.value("description", "");

            if (s.name.empty())
                return ApiUtils::createErrorResponse("name is required", 400, origin);

            if (database::SiteRepository::updateSite(s)) {
                database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "UPDATE_SITE",
                                "Updated site id=" + std::to_string(id));
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
        } catch (const nlohmann::json::exception& e) {
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
        return ApiUtils::createErrorResponse("Failed to update site", 500, origin);
    });
}

} // namespace api
} // namespace vms
