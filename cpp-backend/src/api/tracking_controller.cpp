#include "api/tracking_controller.h"
#include "core/camera_manager.h"
#include "core/camera_pipeline_manager.h"
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

            // Persist into camera.ai_config under "tracking" key. ai_worker spawn args
            // pick this up on next pipeline start; live update is not yet supported.
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(camera_id);
            if (!camera_opt) return ApiUtils::createErrorResponse("Camera not found", 404, origin);

            json ai_cfg = json::object();
            try {
                if (!camera_opt->ai_config.empty()) ai_cfg = json::parse(camera_opt->ai_config);
                if (!ai_cfg.is_object()) ai_cfg = json::object();
            } catch (...) {
                ai_cfg = json::object();
            }

            json& tracking = ai_cfg["tracking"];
            if (!tracking.is_object()) tracking = json::object();
            for (const auto& key : {"tracking_enabled", "confidence_threshold", "max_tracks", "class_filter"}) {
                if (body.contains(key)) tracking[key] = body[key];
            }

            Camera updated = camera_opt.value();
            updated.ai_config = ai_cfg.dump();
            if (!camera_mgr.updateCamera(updated)) {
                return ApiUtils::createErrorResponse("Failed to persist tracking config", 500, origin);
            }

            LOG_INFO("Tracking config persisted for camera {}: {}", camera_id, tracking.dump());
            json resp = {
                {"persisted", true},
                {"applied_on_next_start", true},
                {"tracking", tracking}
            };
            return ApiUtils::createResponse(resp, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // DELETE reset — restart pipeline so the in-process ObjectTracker / FaceTracker
    // state is cleared. There is no live RESET command channel today; the trackers
    // live inside the spawned ai_worker process and only fresh-start clears them.
    CROW_ROUTE(app, "/api/tracking/cameras/<int>/reset")
    .methods(crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(camera_id);
            if (!camera_opt) return ApiUtils::createErrorResponse("Camera not found", 404, origin);

            auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
            const bool was_running = pipeline_mgr.isRunning(camera_id);
            bool restarted = false;

            if (was_running) {
                pipeline_mgr.stopPipeline(camera_id);
                if (!camera_opt->rtsp_url.empty()) {
                    restarted = pipeline_mgr.startPipeline(camera_id, camera_opt->rtsp_url);
                    if (!restarted) {
                        LOG_WARN("Tracking reset: failed to restart pipeline for camera {}", camera_id);
                    }
                } else {
                    LOG_WARN("Tracking reset: camera {} has no rtsp_url, skipping restart", camera_id);
                }
            }

            LOG_INFO("Tracking reset for camera {} (was_running={}, restarted={})",
                     camera_id, was_running, restarted);
            json resp = {
                {"reset", true},
                {"was_running", was_running},
                {"restarted", restarted}
            };
            return ApiUtils::createResponse(resp, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });
}

} // namespace api
} // namespace vms
