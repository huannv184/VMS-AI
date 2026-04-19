#include "api/tracking_controller.h"
#include "core/camera_manager.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include "database/json_serialization.h"

using json = nlohmann::json;

#ifdef DELETE
#undef DELETE
#endif

namespace vms {
namespace api {

void TrackingController::registerRoutes(vms::server::VmsApp& app) {
    // GET active tracks
    CROW_ROUTE(app, "/api/tracking/cameras/<int>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::OPTIONS) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        
        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(camera_id);
            
            if (!camera_opt) {
                return ApiUtils::createErrorResponse("Camera not found", 404, origin);
            }
            
            auto metadata_opt = camera_mgr.getLatestMetadata(camera_id);
            
            json tracks = json::array();
            if (metadata_opt) {
                auto metadata = metadata_opt.value();
                for (const auto& det : metadata.detections) {
                    json track = {
                        {"track_id", det.track_id},
                        {"class_id", det.class_id},
                        {"class_name", det.class_name},
                        {"confidence", det.confidence},
                        {"bbox", {{"x", det.x}, {"y", det.y}, {"width", det.width}, {"height", det.height}}},
                        {"timestamp", metadata.timestamp}
                    };
                    tracks.push_back(track);
                }
            }
            
            return ApiUtils::createResponse({
                {"camera_id", camera_id},
                {"active_tracks", tracks},
                {"count", tracks.size()}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET tracking stats
    CROW_ROUTE(app, "/api/tracking/cameras/<int>/stats")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::OPTIONS) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        
        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(camera_id);
            
            if (!camera_opt) {
                return ApiUtils::createErrorResponse("Camera not found", 404, origin);
            }
            
            auto metadata_opt = camera_mgr.getLatestMetadata(camera_id);
            int active_tracks = 0;
            int total_objects = 0;
            json class_distribution = json::object();
            
            if (metadata_opt) {
                auto metadata = metadata_opt.value();
                active_tracks = metadata.detections.size();
                for (const auto& det : metadata.detections) {
                    total_objects++;
                    std::string cls = det.class_name;
                    class_distribution[cls] = class_distribution.value(cls, 0) + 1;
                }
            }
            
            return ApiUtils::createResponse({
                {"camera_id", camera_id},
                {"active_tracks", active_tracks},
                {"total_objects_detected", total_objects},
                {"class_distribution", class_distribution},
                {"tracking_enabled", camera_opt.value().is_active}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // POST tracking config
    CROW_ROUTE(app, "/api/tracking/cameras/<int>/config")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        
        try {
            auto body = json::parse(req.body);
            
            // Validate Config
            if (body.contains("tracking_enabled") && !body["tracking_enabled"].is_boolean()) {
                return ApiUtils::createErrorResponse("tracking_enabled must be boolean", 400, origin);
            }
            
            if (body.contains("confidence_threshold")) {
                 if (!body["confidence_threshold"].is_number()) return ApiUtils::createErrorResponse("confidence_threshold must be number", 400, origin);
                 double conf = body["confidence_threshold"];
                 if (conf < 0.0 || conf > 1.0) return ApiUtils::createErrorResponse("confidence_threshold must be 0.0-1.0", 400, origin);
            }
            
            if (body.contains("max_tracks")) {
                 if (!body["max_tracks"].is_number_integer()) return ApiUtils::createErrorResponse("max_tracks must be integer", 400, origin);
                 int max_tracks = body["max_tracks"];
                 if (max_tracks < 1 || max_tracks > 1000) return ApiUtils::createErrorResponse("max_tracks must be 1-1000", 400, origin);
            }
            
            if (body.contains("class_filter")) {
                if (!body["class_filter"].is_array()) return ApiUtils::createErrorResponse("class_filter must be array", 400, origin);
                for (const auto& item : body["class_filter"]) {
                    if (!item.is_string()) return ApiUtils::createErrorResponse("class_filter items must be strings", 400, origin);
                }
            }

            // TODO: Send to AI server
            LOG_INFO("Tracking config for camera {}: {}", camera_id, body.dump());
            return ApiUtils::createResponse(json::object(), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // DELETE reset
    CROW_ROUTE(app, "/api/tracking/cameras/<int>/reset")
    .methods(crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        
        try {
             // TODO: Send reset
             LOG_INFO("Tracking reset for camera {}", camera_id);
             return ApiUtils::createResponse(json::object(), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });
}

} // namespace api
} // namespace vms
