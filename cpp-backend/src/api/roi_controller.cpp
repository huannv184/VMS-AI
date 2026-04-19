#include "api/roi_controller.h"
#include "core/roi_manager.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include "database/json_serialization.h"

using json = nlohmann::json;
using vms::api::Permission;

#ifdef DELETE
#undef DELETE
#endif

namespace vms {
namespace api {

void ROIController::registerRoutes(vms::server::VmsApp& app) {
    // GET all ROIs
    CROW_ROUTE(app, "/api/rois")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        
        try {
            auto& roi_mgr = vms::core::ROIManager::getInstance();
            auto rois = roi_mgr.getAllROIs(); 
            
            json rois_arr = json::array();
            for (const auto& roi : rois) {
                rois_arr.push_back(roi);
            }

            return ApiUtils::createResponse({{"rois", rois_arr}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET ROIs by camera
    CROW_ROUTE(app, "/api/roi/cameras/<int>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            auto& roi_mgr = vms::core::ROIManager::getInstance();
            auto rois = roi_mgr.getCameraROIs(camera_id);

            json rois_arr = json::array();
            for (const auto& roi : rois) {
                rois_arr.push_back(roi);
            }

            return ApiUtils::createResponse({{"rois", rois_arr}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // POST create ROI (supports both /api/roi/create and /api/cameras/<id>/roi)
    auto roiCreateHandler = [&app](const crow::request& req, int route_camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ROI_WRITE, origin)) return std::move(*err);

        try {
            LOG_INFO("[ROIController] POST create ROI body: {}", req.body);
            auto body = json::parse(req.body);

            // Support camera_id from route param or body
            int camera_id = route_camera_id;
            if (camera_id <= 0 && body.contains("camera_id")) {
                camera_id = body["camera_id"].get<int>();
            }
            if (camera_id <= 0) {
                return ApiUtils::createErrorResponse("Missing required field: camera_id", 400, origin);
            }

            if (!body.contains("points")) {
                return ApiUtils::createErrorResponse("Missing required field: points", 400, origin);
            }

            if (!body["points"].is_array()) return ApiUtils::createErrorResponse("Points must be an array", 400, origin);
            if (body["points"].size() > 100) return ApiUtils::createErrorResponse("Too many points (max 100)", 400, origin);

            bool has_pixel_coords = false;
            for (const auto& point : body["points"]) {
                if (!point.contains("x") || !point.contains("y")) return ApiUtils::createErrorResponse("Invalid point format (missing x or y)", 400, origin);
                if (!point["x"].is_number() || !point["y"].is_number()) return ApiUtils::createErrorResponse("Coordinates must be numbers", 400, origin);
                double x = point["x"];
                double y = point["y"];
                if (x < 0 || y < 0) return ApiUtils::createErrorResponse("Coordinates cannot be negative", 400, origin);
                // FIX: Warn if coordinates look like pixel values instead of normalized 0..1
                if (x > 1.0 || y > 1.0) has_pixel_coords = true;
            }
            if (has_pixel_coords) {
                LOG_WARN("[ROIController] ROI coordinates > 1.0 detected for camera {} — "
                         "frontend may be sending pixel coordinates instead of normalized [0..1]", camera_id);
            }

            vms::ROI roi;
            roi.camera_id = camera_id;
            roi.name = body.value("name", "New ROI");
            if (roi.name.length() > 100) return ApiUtils::createErrorResponse("Name too long", 400, origin);

            // FIX: read 'roi_type' (sent by frontend api-client) with 'type' as fallback
            if (body.contains("roi_type") && body["roi_type"].is_string())
                roi.roi_type = body["roi_type"].get<std::string>();
            else
                roi.roi_type = body.value("type", "polygon");
            roi.points_json = body["points"].dump();
            roi.enabled = body.value("enabled", true);

            auto& roi_mgr = vms::core::ROIManager::getInstance();
            int inserted_id = roi_mgr.addROI(roi);
            if (inserted_id > 0) {
                LOG_INFO("[ROIController] ROI created: id={} camera={} name='{}'", inserted_id, camera_id, roi.name);
                // FIX: Return the created ROI with its ID so the frontend can reference it
                roi.id = inserted_id;
                json response = roi;
                return ApiUtils::createResponse(response, 201, origin);
            }

            LOG_ERROR("[ROIController] Failed to create ROI for camera {}", camera_id);
            return ApiUtils::createErrorResponse("Failed to create ROI", 500, origin);
        } catch (const json::exception& e) {
            LOG_ERROR("[ROIController] Invalid JSON: {}", e.what());
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            LOG_ERROR("[ROIController] Exception: {}", e.what());
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    };

    CROW_ROUTE(app, "/api/roi/create")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([roiCreateHandler](const crow::request& req) {
        return roiCreateHandler(req, 0);
    });

    // FIX: Compatibility route for frontends that use /api/cameras/<id>/roi
    CROW_ROUTE(app, "/api/cameras/<int>/roi")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([roiCreateHandler](const crow::request& req, int camera_id) {
        return roiCreateHandler(req, camera_id);
    });

    // /api/roi/<int> (GET, PUT, DELETE)
    CROW_ROUTE(app, "/api/roi/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        // Mutating methods require ROI_WRITE; GET only requires ROI_READ (implied by auth)
        if (req.method != crow::HTTPMethod::Get) {
            if (auto err = ApiUtils::requirePermission(ctx, Permission::ROI_WRITE, origin)) return std::move(*err);
        }

        auto& roi_mgr = vms::core::ROIManager::getInstance();

        if (req.method == crow::HTTPMethod::Get) {
            try {
                auto roi_opt = roi_mgr.getROI(id);
                if (roi_opt) return ApiUtils::createResponse(roi_opt.value(), 200, origin);
                return ApiUtils::createErrorResponse("ROI not found", 404, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        if (req.method == crow::HTTPMethod::Put) {
            try {
                auto body = json::parse(req.body);
                auto roi_opt = roi_mgr.getROI(id);
                
                if (!roi_opt) return ApiUtils::createErrorResponse("ROI not found", 404, origin);
                
                vms::ROI roi = roi_opt.value();
                if (body.contains("name")) roi.name = body["name"];
                if (body.contains("points")) roi.points_json = body["points"].dump();
                if (body.contains("enabled")) roi.enabled = body["enabled"];
                // FIX: read 'roi_type' with 'type' as fallback
                if (body.contains("roi_type") && body["roi_type"].is_string())
                    roi.roi_type = body["roi_type"].get<std::string>();
                else if (body.contains("type")) roi.roi_type = body["type"];
                
                if (roi_mgr.updateROI(roi)) return ApiUtils::createResponse(json::object(), 200, origin);
                return ApiUtils::createErrorResponse("Failed to update ROI", 500, origin);
            } catch (const json::exception& e) {
                return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        if (req.method == crow::HTTPMethod::Delete) {
            try {
                if (roi_mgr.deleteROI(id)) return ApiUtils::createResponse(json::object(), 200, origin);
                return ApiUtils::createErrorResponse("ROI not found or failed to delete", 404, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });
    
    // GET ROI stats
    CROW_ROUTE(app, "/api/roi/stats")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
             auto& roi_mgr = vms::core::ROIManager::getInstance();
             auto rois = roi_mgr.getAllROIs();
             
             json response = {
                 {"total_rois", rois.size()}
             };
             
             return ApiUtils::createResponse(response, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });
}

} // namespace api
} // namespace vms
