#include "../../include/api/camera_controller.h"
#include "core/camera_manager.h"
#include "core/camera_pipeline_manager.h"
#include "core/camera_event_service.h"
#include "streaming/camera_stream_manager_qt.h"
#include "utils/api_utils.h"
#include "utils/config.h"
#include "utils/logger.h"
#include "database/audit_repository.h"
#include "database/permission_repository.h"
#include "database/json_serialization.h"
#include "middleware/auth_middleware.h"
#include "server/vms_app.h"
#include "utils/validator.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <crow.h>
#include "core/camera_types.h"
#include "utils/csv_parser.hpp"
#include <sstream>

using json = nlohmann::json;

#ifdef DELETE
#undef DELETE
#endif

namespace vms {
namespace api {

namespace {

std::atomic<uint64_t> g_camera_list_cache_version{1};
std::mutex g_camera_list_cache_mutex;
std::shared_ptr<const std::string> g_public_camera_list_cache;
std::shared_ptr<const std::string> g_admin_camera_list_cache;
std::atomic<int64_t> g_public_camera_list_cache_deadline_ms{0};
std::atomic<int64_t> g_admin_camera_list_cache_deadline_ms{0};

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void invalidateCameraListCache() {
    g_camera_list_cache_version.fetch_add(1, std::memory_order_acq_rel);
    g_public_camera_list_cache_deadline_ms.store(0, std::memory_order_release);
    g_admin_camera_list_cache_deadline_ms.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_camera_list_cache_mutex);
    std::atomic_store_explicit(&g_public_camera_list_cache,
                               std::shared_ptr<const std::string>{},
                               std::memory_order_release);
    std::atomic_store_explicit(&g_admin_camera_list_cache,
                               std::shared_ptr<const std::string>{},
                               std::memory_order_release);
}

std::string trimCopy(const std::string& input) {
    auto begin = std::find_if_not(input.begin(), input.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();
    if (begin >= end) return "";
    return std::string(begin, end);
}

std::optional<std::string> readOptionalString(const json& body, const char* key) {
    if (!body.contains(key) || body[key].is_null()) {
        return std::nullopt;
    }
    if (!body[key].is_string()) {
        throw std::runtime_error(std::string("Field must be a string: ") + key);
    }
    return trimCopy(body[key].get<std::string>());
}

std::optional<std::string> assignValidatedString(const json& body,
                                                 const char* key,
                                                 std::string& target,
                                                 size_t max_length,
                                                 const char* invalid_message) {
    auto value = readOptionalString(body, key);
    target = value.value_or("");
    if (!target.empty() && !vms::Validator::isValidString(target, max_length)) {
        return invalid_message;
    }
    return std::nullopt;
}

std::optional<std::string> assignValidatedRtsp(const json& body,
                                               const char* key,
                                               std::string& target,
                                               bool required,
                                               const char* missing_message,
                                               const char* invalid_message) {
    auto value = readOptionalString(body, key);
    if (required && !value.has_value()) {
        return missing_message;
    }
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (value->empty()) {
        target.clear();
        return std::nullopt;
    }
    auto normalized = vms::Validator::normalizeRtspUrl(*value);
    if (!normalized.has_value()) {
        return invalid_message;
    }
    target = normalized.value();
    return std::nullopt;
}

void appendCameraRuntimeFields(json& target,
                               const vms::core::CameraStats& stats,
                               bool camera_active,
                               const char* stopped_status) {
    target["fps"] = stats.is_running ? stats.fps : 0;
    target["restart_count"] = stats.restart_count;
    target["last_frame_time"] = stats.last_frame_ts;
    target["status"] = stats.is_running ? "online" : (camera_active ? stopped_status : "inactive");
    target["bitrate_kbps"] = stats.is_running ? static_cast<int>(stats.fps * 150) : 0;

    if (!stats.is_running) {
        target["cpu_usage_percent"] = 0;
    } else if (stats.cpu_usage_percent < 0) {
        target["cpu_usage_percent"] = nullptr;
    } else {
        target["cpu_usage_percent"] = std::round(stats.cpu_usage_percent * 10.0) / 10.0;
    }

    target["uptime_seconds"] = stats.is_running ? stats.last_frame_ts : 0;
}

void appendUnavailableCameraRuntimeFields(json& target) {
    target["fps"] = 0;
    target["restart_count"] = 0;
    target["last_frame_time"] = 0;
    target["status"] = "error";
    target["bitrate_kbps"] = 0;
    target["cpu_usage_percent"] = nullptr;
    target["uptime_seconds"] = 0;
}

std::optional<std::string> validateCameraPayload(vms::Camera& camera, const json& body, bool is_update) {
    if (!is_update || body.contains("name")) {
        if (auto err = assignValidatedString(body, "name", camera.name, 120, "Invalid camera name")) {
            return err;
        }
    }

    if (!is_update || body.contains("rtsp_url")) {
        if (auto err = assignValidatedRtsp(body, "rtsp_url", camera.rtsp_url, true,
                                           "Missing required field: rtsp_url",
                                           "Invalid RTSP URL")) {
            return err;
        }
    }

    if (body.contains("sub_stream_url")) {
        if (auto err = assignValidatedRtsp(body, "sub_stream_url", camera.sub_stream_url, false,
                                           "",
                                           "Invalid sub stream RTSP URL")) {
            return err;
        }
    }

    if (body.contains("description")) {
        if (auto err = assignValidatedString(body, "description", camera.description, 500,
                                             "Invalid camera description")) {
            return err;
        }
    }

    if (body.contains("is_active")) {
        if (!body["is_active"].is_boolean()) {
            return "Field must be boolean: is_active";
        }
        camera.is_active = body["is_active"].get<bool>();
    }

    if (body.contains("ai_config")) {
        if (body["ai_config"].is_string()) {
            camera.ai_config = body["ai_config"];
        } else if (body["ai_config"].is_object() || body["ai_config"].is_array()) {
            camera.ai_config = body["ai_config"].dump();
        } else {
            return "Invalid ai_config";
        }
    }

    if (body.contains("site_id")) {
        if (!body["site_id"].is_number()) {
            return "Field must be a number: site_id";
        }
        camera.site_id = body["site_id"].get<int>();
    }

    return std::nullopt;
}

}

void CameraController::registerRoutes(vms::server::VmsApp& app) {
    LOG_INFO("Registering camera routes via controller...");

    // /api/cameras (GET, POST)
    CROW_ROUTE(app, "/api/cameras")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        const bool auth_enabled = vms::Config::getInstance().getAuthConfig().enabled;
        if (auth_enabled && !ctx.user.has_value()) {
            return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        }
        
        // GET all cameras
        if (req.method == crow::HTTPMethod::Get) {
            if (auth_enabled) {
                if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_READ, origin)) return std::move(*err);
            }

            const bool can_use_admin_cache = auth_enabled && ctx.user->role_id == 1;
            const auto now_ms = steadyNowMs();
            auto& cache_deadline = can_use_admin_cache
                ? g_admin_camera_list_cache_deadline_ms
                : g_public_camera_list_cache_deadline_ms;
            auto* cache_body_ptr = can_use_admin_cache
                ? &g_admin_camera_list_cache
                : &g_public_camera_list_cache;
            auto cached_body = std::atomic_load_explicit(cache_body_ptr, std::memory_order_acquire);
            if ((!auth_enabled || can_use_admin_cache) &&
                now_ms < cache_deadline.load(std::memory_order_acquire) &&
                cached_body && !cached_body->empty()) {
                crow::response res;
                res.code = 200;
                res.set_header("Content-Type", "application/json");
                ApiUtils::applyCors(res, origin);
                res.body = *cached_body;
                return res;
            }

            try {
                auto& camera_mgr = vms::core::CameraManager::getInstance();
                auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
                const auto cache_version = g_camera_list_cache_version.load(std::memory_order_acquire);

                auto cameras = !auth_enabled || ctx.user->role_id == 1
                    ? camera_mgr.getAllCameras()
                    : camera_mgr.getRepository().getFilteredCameras(ctx.user->id);
                
                json cam_list = json::array();
                for (const auto& cam : cameras) {
                    json c = cam;
                    
                    try {
                        appendCameraRuntimeFields(
                            c,
                            pipeline_mgr.getCameraStats(cam.id),
                            cam.is_active,
                            "error");
                    } catch (const std::exception& e) {
                        LOG_ERROR("Error getting stats for camera {}: {}", cam.id, e.what());
                        appendUnavailableCameraRuntimeFields(c);
                    }

                    c["resolution"] = "1920x1080"; 
                    c["codec"] = "H.264";
                    cam_list.push_back(c);
                }

                auto response = ApiUtils::createResponse({{"cameras", cam_list}}, 200, origin);
                if ((!auth_enabled || can_use_admin_cache) &&
                    cache_version == g_camera_list_cache_version.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lock(g_camera_list_cache_mutex);
                    std::atomic_store_explicit(
                        cache_body_ptr,
                        std::make_shared<const std::string>(response.body),
                        std::memory_order_release);
                    cache_deadline.store(now_ms + 1000, std::memory_order_release);
                }
                return response;
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        }
        
        // POST create camera (requires auth)
        if (req.method == crow::HTTPMethod::Post) {
            if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_WRITE, origin)) return std::move(*err);
            try {
                auto body = json::parse(req.body);

                vms::Camera camera;
                auto validation_error = validateCameraPayload(camera, body, false);
                if (validation_error.has_value()) {
                    LOG_WARN("[CameraController] Validation failed: {}", validation_error.value());
                    return ApiUtils::createErrorResponse(validation_error.value(), 400, origin);
                }

                auto& camera_mgr = vms::core::CameraManager::getInstance();
                if (auto duplicate = camera_mgr.getRepository().findByNameOrRtsp(camera.name, camera.rtsp_url);
                    duplicate.has_value()) {
                    LOG_WARN("[CameraController] Duplicate camera: name='{}' url='{}'", camera.name, camera.rtsp_url);
                    // Give a more helpful message: if the duplicate is invisible to this user,
                    // they may not know it exists. Admins always see everything; non-admins may not.
                    const bool user_can_see_duplicate = !auth_enabled || ctx.user->role_id == 1;
                    if (!user_can_see_duplicate) {
                        return ApiUtils::createErrorResponse(
                            "Camera already exists in the system. Contact an administrator to grant access.", 409, origin);
                    }
                    return ApiUtils::createErrorResponse("Camera already exists with the same name or RTSP URL", 409, origin);
                }

                if (camera_mgr.addCamera(camera)) {
                    invalidateCameraListCache();
                    LOG_INFO("[CameraController] Camera added: id={} name='{}' url='{}'",
                             camera.id, camera.name, camera.rtsp_url);

                    int caller_id = ctx.user->id;
                    vms::database::AuditRepository audit;
                    audit.insertLog(caller_id, "CREATE_CAMERA", "Created camera: " + camera.name);

                    // Auto-grant permission to the creating operator so they can see what they added
                    if (auth_enabled && ctx.user->role_id != 1) {
                        auto perms = vms::database::PermissionRepository::getPermissions(caller_id);
                        perms.allowed_camera_ids.push_back(camera.id);
                        vms::database::PermissionRepository::setPermissions(perms);
                        LOG_INFO("[CameraController] Granted camera {} access to user {}", camera.id, caller_id);
                    }

                // Keep POST aligned with update flow: active cameras should boot immediately.
                json response_json = camera;
                auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
                if (camera.is_active) {
                        if (pipeline_mgr.startPipeline(camera.id, camera.rtsp_url)) {
                            LOG_INFO("[CameraController] Pipeline started for new camera {}", camera.id);
                            response_json["pipeline_status"] = "started";
                        } else {
                            LOG_WARN("[CameraController] Pipeline failed to start for camera {}", camera.id);
                            response_json["pipeline_status"] = "pending";
                        }
                    } else {
                        response_json["pipeline_status"] = "inactive";
                    }

                    return ApiUtils::createResponse(response_json, 201, origin);
                }

                LOG_ERROR("[CameraController] addCamera() returned false for name='{}'", camera.name);
                return ApiUtils::createErrorResponse("Failed to create camera", 500, origin);
            } catch (const json::exception& e) {
                return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // /api/cameras/<int> (GET, PUT, DELETE)
    CROW_ROUTE(app, "/api/cameras/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) {
            return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        }
        
        auto& camera_mgr = vms::core::CameraManager::getInstance();

        // GET camera by ID
        if (req.method == crow::HTTPMethod::Get) {
            if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_READ, origin)) return std::move(*err);
            try {
                auto camera = camera_mgr.getCamera(id);
                if (camera) {
                    auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
                    json c = camera.value();
                    
                    try {
                        auto stats = pipeline_mgr.getCameraStats(id);
                        c["fps"] = stats.is_running ? stats.fps : 0;
                        c["status"] = stats.is_running ? "online" : (camera.value().is_active ? "offline" : "inactive");
                    } catch (...) {
                         c["fps"] = 0;
                         c["status"] = "error";
                    }
                    
                    c["resolution"] = "1920x1080";
                    
                    return ApiUtils::createResponse(c, 200, origin);
                }
                return ApiUtils::createErrorResponse("Camera not found", 404, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        }
        
        // PUT update camera (requires auth)
        if (req.method == crow::HTTPMethod::Put) {
            if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_WRITE, origin)) return std::move(*err);
            try {
                auto body = json::parse(req.body);
                auto camera_opt = camera_mgr.getCamera(id);
                
                if (!camera_opt) {
                    return ApiUtils::createErrorResponse("Camera not found", 404, origin);
                }
                
                vms::Camera camera = camera_opt.value();

                auto validation_error = validateCameraPayload(camera, body, true);
                if (validation_error.has_value()) {
                    return ApiUtils::createErrorResponse(validation_error.value(), 400, origin);
                }

                if (auto duplicate = camera_mgr.getRepository().findByNameOrRtsp(camera.name, camera.rtsp_url, id);
                    duplicate.has_value()) {
                    return ApiUtils::createErrorResponse("Camera already exists with the same name or RTSP URL", 409, origin);
                }
                
                if (camera_mgr.updateCamera(camera)) {
                    invalidateCameraListCache();
                    // Restart pipeline if camera is active
                    auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
                    if (camera.is_active) {
                        pipeline_mgr.stopPipeline(camera.id);
                        pipeline_mgr.startPipeline(camera.id, camera.rtsp_url);
                    } else {
                        pipeline_mgr.stopPipeline(camera.id);
                    }
                    
                    int caller_id = ctx.user->id;
                    vms::database::AuditRepository audit;
                    audit.insertLog(caller_id, "UPDATE_CAMERA", "Updated camera ID: " + std::to_string(id));
                    
                    return ApiUtils::createResponse(camera, 200, origin);
                }
                
                return ApiUtils::createErrorResponse("Failed to update camera", 500, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createSafeError(e, 500, origin);
            }
        }
        // DELETE camera (requires auth)
        if (req.method == crow::HTTPMethod::Delete) {
            if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_WRITE, origin)) return std::move(*err);
            try {
                LOG_INFO("API: Request to delete camera ID: {}", id);
                if (camera_mgr.removeCamera(id)) {
                    invalidateCameraListCache();
                    LOG_INFO("API: Camera {} deleted successfully", id);
                    
                    int caller_id = ctx.user->id;
                    vms::database::AuditRepository audit;
                    audit.insertLog(caller_id, "DELETE_CAMERA", "Deleted camera ID: " + std::to_string(id));
                    
                    return ApiUtils::createResponse(json::object(), 200, origin);
                }
                LOG_ERROR("API: Failed to delete camera ID {}", id);
                return ApiUtils::createErrorResponse("Camera not found or failed to delete", 404, origin);
            } catch (const std::exception& e) {
                LOG_ERROR("API: Exception deleting camera {}: {}", id, e.what());
                return ApiUtils::createSafeError(e, 500, origin);
            } catch (...) {
                LOG_ERROR("API: Unknown exception deleting camera {}", id);
                return ApiUtils::createErrorResponse("Internal server error", 500, origin);
            }
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // GET /api/cameras/<int>/frame
    CROW_ROUTE(app, "/api/cameras/<int>/frame")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) {
            return ApiUtils::createResponse(nlohmann::json::object(), 204, origin);
        }
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_READ, origin)) return std::move(*err);

        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(id);
            
            if (!camera_opt) {
                return ApiUtils::createErrorResponse("Camera not found", 404, origin);
            }
            
            auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
            if (!pipeline_mgr.isRunning(id)) {
                return ApiUtils::createErrorResponse("Pipeline not running", 503, origin);
            }
            
            auto frame_opt = pipeline_mgr.getLatestFrame(id);
            if (!frame_opt || frame_opt.value().empty()) {
                return ApiUtils::createErrorResponse("Stream connected but no frames yet", 503, origin);
            }
            
            const auto& frame_data = frame_opt.value();
            crow::response res;
            res.code = 200;
            res.set_header("Content-Type", "image/jpeg");
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            res.set_header("X-Metadata", pipeline_mgr.getLatestObjectsJson(id));
            res.set_header("Content-Length", std::to_string(frame_data.size()));
            res.body.reserve(frame_data.size());
            res.body.assign(frame_data.begin(), frame_data.end());
            return res;
        } catch (const std::exception& e) {
            LOG_ERROR("Error serving frame for camera {}: {}", id, e.what());
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // GET /api/cameras/<int>/metadata
    CROW_ROUTE(app, "/api/cameras/<int>/metadata")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_READ, origin)) return std::move(*err);

        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(id);
            if (!camera_opt) {
                return ApiUtils::createErrorResponse("Camera not found", 404, origin);
            }

            auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
            json metadata = pipeline_mgr.getLatestMetadata(id);
            if (!metadata.is_object()) {
                metadata = json::object();
            }
            if (!metadata.contains("camera_id")) {
                metadata["camera_id"] = id;
            }
            if (!metadata.contains("objects")) {
                try {
                    metadata["objects"] = json::parse(pipeline_mgr.getLatestObjectsJson(id));
                } catch (...) {
                    metadata["objects"] = json::array();
                }
            }
            return ApiUtils::createResponse(metadata, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // GET/POST /api/cameras/<int>/snapshot
    CROW_ROUTE(app, "/api/cameras/<int>/snapshot")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_READ, origin)) return std::move(*err);

        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(id);
            if (!camera_opt) {
                return ApiUtils::createErrorResponse("Camera not found", 404, origin);
            }

            auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
            auto frame_opt = pipeline_mgr.getLatestFrame(id);
            if (!frame_opt || frame_opt->empty()) {
                return ApiUtils::createErrorResponse("No snapshot available", 503, origin);
            }

            crow::response res;
            res.code = 200;
            res.set_header("Content-Type", "image/jpeg");
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            ApiUtils::applyCors(res, origin);
            res.body.assign(frame_opt->begin(), frame_opt->end());
            return res;
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // POST start camera
    CROW_ROUTE(app, "/api/cameras/<int>/start")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_WRITE, origin)) return std::move(*err);
        
        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            if (camera_mgr.startCamera(id)) {
                invalidateCameraListCache();
                vms::database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "START_CAMERA", "Started camera ID: " + std::to_string(id));
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
            
            return ApiUtils::createErrorResponse("Failed to start camera", 500, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // POST stop camera
    CROW_ROUTE(app, "/api/cameras/<int>/stop")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_WRITE, origin)) return std::move(*err);
        
        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            if (camera_mgr.stopCamera(id)) {
                invalidateCameraListCache();
                vms::database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "STOP_CAMERA", "Stopped camera ID: " + std::to_string(id));
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
            
            return ApiUtils::createErrorResponse("Failed to stop camera", 500, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // GET camera status
    CROW_ROUTE(app, "/api/cameras/<int>/status")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_READ, origin)) return std::move(*err);
        
        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(id);
            if (!camera_opt) return ApiUtils::createErrorResponse("Camera not found", 404, origin);
            bool is_active = camera_opt.value().is_active;
            json response = {{"id", id}, {"is_active", is_active}, {"status", is_active ? "running" : "stopped"}};
            return ApiUtils::createResponse(response, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // POST /api/cameras/:id/refresh-advanced
    CROW_ROUTE(app, "/api/cameras/<int>/refresh-advanced")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_WRITE, origin)) return std::move(*err);
        
        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            if (camera_mgr.refreshAdvancedConfig(id)) {
                invalidateCameraListCache();
                vms::database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "REFRESH_CAMERA_ADVANCED", "Refreshed advanced config for camera ID: " + std::to_string(id));
                auto cam_opt = camera_mgr.getCamera(id);
                if (cam_opt) return ApiUtils::createResponse(cam_opt.value(), 200, origin);
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
            return ApiUtils::createErrorResponse("Failed to refresh advanced config", 500, origin);
        } catch (const std::exception& e) { return ApiUtils::createSafeError(e, 500, origin); }
    });

    // GET /api/cameras/:id/stats
    CROW_ROUTE(app, "/api/cameras/<int>/stats")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_READ, origin)) return std::move(*err);
        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(id);
            if (!camera_opt) return ApiUtils::createErrorResponse("Camera not found", 404, origin);

            auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
            json stats_json;
            try {
                auto stats = pipeline_mgr.getCameraStats(id);
                appendCameraRuntimeFields(stats_json, stats, camera_opt.value().is_active, "offline");
                stats_json["is_running"] = stats.is_running;

            } catch (...) {
                appendUnavailableCameraRuntimeFields(stats_json);
                stats_json["is_running"] = false;
            }
            stats_json["id"] = id;
            stats_json["resolution"] = "1920x1080";
            return ApiUtils::createResponse(stats_json, 200, origin);
        } catch (const std::exception& e) { return ApiUtils::createSafeError(e, 500, origin); }
    });

    // GET /api/cameras/:id/event_subscription_status — current hardware-event
    // subscription health so operators can tell "quiet scene" from
    // "subscription/auth/backend problem".
    CROW_ROUTE(app, "/api/cameras/<int>/event_subscription_status")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_READ, origin)) return std::move(*err);
        try {
            return ApiUtils::createResponse(
                vms::core::CameraEventService::getInstance().getStatus(std::to_string(id)),
                200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // POST /api/cameras/import — batch import via JSON or CSV (Requirement 1.3)
    CROW_ROUTE(app, "/api/cameras/import")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::CAMERA_WRITE, origin)) return std::move(*err);

        try {
            json cameras_to_import = json::array();
            std::string content_type = req.get_header_value("Content-Type");
            
            if (content_type.find("text/csv") != std::string::npos || (req.body.find(",") != std::string::npos && req.body.find("{") == std::string::npos)) {
                cameras_to_import = vms::utils::CsvParser::parseCameras(req.body);
            } else {
                auto body = json::parse(req.body);
                if (body.is_array()) cameras_to_import = body;
                else if (body.is_object() && body.contains("cameras")) cameras_to_import = body["cameras"];
            }

            if (cameras_to_import.empty()) return ApiUtils::createErrorResponse("No valid cameras identified", 400, origin);

            auto& camera_mgr = vms::core::CameraManager::getInstance();
            int success = 0; int fail = 0; json errors = json::array();

            for (const auto& cam_json : cameras_to_import) {
                try {
                    vms::Camera cam;
                    cam.name = cam_json.value("name", "Imported Cam");
                    cam.rtsp_url = cam_json.value("rtsp_url", "");
                    cam.sub_stream_url = cam_json.value("sub_stream_url", "");
                    cam.description = cam_json.value("description", "Batch imported");
                    cam.is_active = cam_json.value("is_active", true);

                    if (!cam.rtsp_url.empty() && camera_mgr.addCamera(cam)) success++;
                    else { fail++; errors.push_back({{"name", cam.name}, {"error", "Failed to add (duplicate or missing URL)"}}); }
                } catch (...) { fail++; }
            }

            if (success > 0) {
                invalidateCameraListCache();
            }

            vms::database::AuditRepository audit;
            audit.insertLog(ctx.user->id, "BATCH_IMPORT", "Imported " + std::to_string(success) + " cameras");
            return ApiUtils::createResponse({{"imported", success}, {"failed", fail}, {"errors", errors}}, 200, origin);
        } catch (const std::exception& e) { return ApiUtils::createSafeError(e, 500, origin); }
    });

    LOG_INFO("Camera controller routes registered - plus snapshot/ptz/stream/import");
}

} // namespace api
} // namespace vms
