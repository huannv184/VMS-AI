#include "api/anpr_controller.h"
#include "database/anpr_repository.h"
#include "database/audit_repository.h"
#include "middleware/auth_middleware.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include "database/json_serialization.h"

using json = nlohmann::json;

namespace vms {
namespace api {

void ANPRController::registerRoutes(vms::server::VmsApp& app) {

    // Plate history is an analytics/PII surface. GET stays on ANALYTICS_READ;
    // manual POST/DELETE are SYSTEM_ADMIN-only and audited.

    // GET & POST plates
    CROW_ROUTE(app, "/api/anpr/plates")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        Permission needed = Permission::ANALYTICS_READ;
        if (req.method == crow::HTTPMethod::Post || req.method == crow::HTTPMethod::Delete) {
            needed = Permission::SYSTEM_ADMIN;
        }
        if (auto err = ApiUtils::requirePermission(ctx, needed, origin)) return std::move(*err);

        if (req.method == crow::HTTPMethod::Get) {
            try {
                // FIX: Validate and clamp limit/offset to prevent DoS
                int limit = 100;
                int offset = 0;
                // BUG-06 FIX: Wrap stoi in try-catch to return 400 on bad input
                try {
                    if (req.url_params.get("limit")) limit = std::stoi(req.url_params.get("limit"));
                    if (req.url_params.get("offset")) offset = std::stoi(req.url_params.get("offset"));
                } catch (const std::exception&) {
                    return ApiUtils::createErrorResponse("Invalid query parameter: limit and offset must be integers", 400, origin);
                }
                if (limit < 1 || limit > 1000) limit = 100;
                if (offset < 0) offset = 0;
                
                database::ANPRRepository repo;
                auto plates = repo.getAllPlates(limit, offset);
                int total = repo.getPlateCount();
                
                json response_plates = json::array();
                for (const auto& p : plates) response_plates.push_back(p);
                
                json response = {
                    {"plates", response_plates},
                    {"total", total}
                };
                return ApiUtils::createResponse(response, 200, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        } else if (req.method == crow::HTTPMethod::Post) {
            try {
                auto body = json::parse(req.body);
                LicensePlate p;
                if (body.contains("plate_number")) {
                    if (!body["plate_number"].is_string()) return ApiUtils::createErrorResponse("Plate number must be string", 400, origin);
                    p.plate_number = body["plate_number"];
                    
                    // Validation: Length and Format
                    if (p.plate_number.length() < 2 || p.plate_number.length() > 20) {
                         return ApiUtils::createErrorResponse("Invalid plate number length", 400, origin);
                    }
                } else {
                     return ApiUtils::createErrorResponse("Missing plate_number", 400, origin);
                }
                
                if (body.contains("vehicle_type")) {
                     if(body["vehicle_type"].is_string()) p.vehicle_type = body["vehicle_type"];
                     if(p.vehicle_type.length() > 50) return ApiUtils::createErrorResponse("Vehicle type too long", 400, origin);
                }
                if (body.contains("color")) {
                     if(body["color"].is_string()) p.color = body["color"];
                     if(p.color.length() > 30) return ApiUtils::createErrorResponse("Color too long", 400, origin);
                }
                if (body.contains("camera_id")) {
                    if (body["camera_id"].is_number_integer()) {
                        int cid = body["camera_id"].get<int>();
                        // Reject pathological values rather than letting them
                        // orphan a plate row to a non-existent camera id.
                        if (cid < 0 || cid > 1000000) {
                            return ApiUtils::createErrorResponse("camera_id out of range", 400, origin);
                        }
                        p.camera_id = cid;
                    }
                }
                if (body.contains("confidence")) {
                     if(body["confidence"].is_number()) p.confidence = body["confidence"];
                     if(p.confidence < 0.0 || p.confidence > 1.0) return ApiUtils::createErrorResponse("Confidence must be 0.0-1.0", 400, origin);
                }

                p.detected_at = std::time(nullptr);

                database::ANPRRepository repo;
                if (repo.insertPlate(p)) {
                    if (ctx.user) {
                        vms::database::AuditRepository audit;
                        audit.insertLog(ctx.user->id, "CREATE_PLATE",
                                        "Plate '" + p.plate_number + "' camera=" + std::to_string(p.camera_id));
                    }
                    return ApiUtils::createResponse(json::object(), 201, origin);
                }
                return ApiUtils::createErrorResponse("Failed to insert plate", 500, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        } else if (req.method == crow::HTTPMethod::Delete) {
            try {
                database::ANPRRepository repo;
                int prior_count = 0;
                try { prior_count = repo.getPlateCount(); } catch (...) { /* best-effort for audit */ }

                if (repo.clearAllPlates()) {
                    if (ctx.user) {
                        vms::database::AuditRepository audit;
                        audit.insertLog(ctx.user->id, "CLEAR_ALL_PLATES",
                                        "Wiped ANPR plate table (prior count=" +
                                        std::to_string(prior_count) + ")");
                    }
                    LOG_WARN("ANPR: clearAllPlates by user_id={} (prior count={})",
                             ctx.user ? ctx.user->id : -1, prior_count);
                    return ApiUtils::createResponse(json::object(), 200, origin);
                }
                return ApiUtils::createErrorResponse("Failed to delete plates", 500, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        }
        
        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // SEARCH plates
    CROW_ROUTE(app, "/api/anpr/search")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);

        try {
            std::string query = "";
            if (req.url_params.get("q")) query = req.url_params.get("q");
            // Limit query length to prevent abuse
            if (query.length() > 100) query = query.substr(0, 100);
            
            database::ANPRRepository repo;
            auto plates = repo.searchPlates(query);
            
            json results = json::array();
            for (const auto& p : plates) results.push_back(p);
            return ApiUtils::createResponse({{"plates", results}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });
}


} // namespace api
} // namespace vms
