#include "api/anpr_controller.h"
#include "database/anpr_repository.h"
#include "utils/api_utils.h"
#include "database/json_serialization.h"

using json = nlohmann::json;

namespace vms {
namespace api {

void ANPRController::registerRoutes(vms::server::VmsApp& app) {
    
    // GET & POST plates
    CROW_ROUTE(app, "/api/anpr/plates")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
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
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
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
                     if(body["camera_id"].is_number_integer()) p.camera_id = body["camera_id"];
                }
                if (body.contains("confidence")) {
                     if(body["confidence"].is_number()) p.confidence = body["confidence"];
                     if(p.confidence < 0.0 || p.confidence > 1.0) return ApiUtils::createErrorResponse("Confidence must be 0.0-1.0", 400, origin);
                }
                
                p.detected_at = std::time(nullptr);
                
                database::ANPRRepository repo;
                if (repo.insertPlate(p)) return ApiUtils::createResponse(json::object(), 201, origin);
                return ApiUtils::createErrorResponse("Failed to insert plate", 500, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        } else if (req.method == crow::HTTPMethod::Delete) {
            try {
                database::ANPRRepository repo;
                if (repo.clearAllPlates()) {
                    return ApiUtils::createResponse(json::object(), 200, origin);
                }
                return ApiUtils::createErrorResponse("Failed to delete plates", 500, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }
        
        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // SEARCH plates
    CROW_ROUTE(app, "/api/anpr/search")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
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
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });
}


} // namespace api
} // namespace vms
