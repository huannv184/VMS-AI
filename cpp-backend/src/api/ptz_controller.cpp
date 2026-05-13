#include "api/ptz_controller.h"
#include "core/ptz_manager.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <optional>

using json = nlohmann::json;
using vms::api::Permission;

namespace vms {
namespace api {

namespace {

std::optional<int> parsePresetIdentifier(const json& body) {
    if (body.contains("preset_id") && body["preset_id"].is_number_integer()) {
        return body["preset_id"].get<int>();
    }

    if (body.contains("preset_token") && body["preset_token"].is_string()) {
        std::string token = body["preset_token"].get<std::string>();
        auto first_digit = std::find_if(token.begin(), token.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        });
        if (first_digit != token.end()) {
            return std::stoi(std::string(first_digit, token.end()));
        }
    }

    return std::nullopt;
}

} // namespace

void PTZController::registerRoutes(vms::server::VmsApp& app) {

    // POST /api/ptz/<cam_id>/move — Continuous move
    CROW_ROUTE(app, "/api/ptz/<int>/move")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            float pan = body.value("pan", 0.0f);
            float tilt = body.value("tilt", 0.0f);
            float zoom = body.value("zoom", 0.0f);

            // Clamp values to -1.0 to 1.0
            pan = std::max(-1.0f, std::min(1.0f, pan));
            tilt = std::max(-1.0f, std::min(1.0f, tilt));
            zoom = std::max(-1.0f, std::min(1.0f, zoom));

            auto& ptz = core::PTZManager::getInstance();
            bool ok = ptz.continuousMove(camera_id, pan, tilt, zoom);

            if (!ok) return ApiUtils::createErrorResponse("PTZ move failed", 500, origin);
            return ApiUtils::createResponse({
                {"camera_id", camera_id},
                {"pan", pan}, {"tilt", tilt}, {"zoom", zoom}
            }, 200, origin);
        } catch (const json::exception& e) {
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // POST /api/ptz/<cam_id>/stop — Stop movement
    CROW_ROUTE(app, "/api/ptz/<int>/stop")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return std::move(*err);

        auto& ptz = core::PTZManager::getInstance();
        bool ok = ptz.stopMove(camera_id);
        if (!ok) return ApiUtils::createErrorResponse("PTZ stop failed", 500, origin);
        return ApiUtils::createResponse(json::object(), 200, origin);
    });

    // POST /api/ptz/<cam_id>/absolute — Absolute position
    CROW_ROUTE(app, "/api/ptz/<int>/absolute")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return std::move(*err);
        
        try {
            auto body = json::parse(req.body);
            float pan = body.value("pan", 0.0f);
            float tilt = body.value("tilt", 0.0f);
            float zoom = body.value("zoom", 0.0f);

            auto& ptz = core::PTZManager::getInstance();
            bool ok = ptz.absoluteMove(camera_id, pan, tilt, zoom);
            if (!ok) return ApiUtils::createErrorResponse("PTZ absolute move failed", 500, origin);
            return ApiUtils::createResponse(json::object(), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // POST /api/ptz/<cam_id>/relative — Relative movement
    CROW_ROUTE(app, "/api/ptz/<int>/relative")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return std::move(*err);
        
        try {
            auto body = json::parse(req.body);
            float pan = body.value("pan", 0.0f);
            float tilt = body.value("tilt", 0.0f);
            float zoom = body.value("zoom", 0.0f);

            auto& ptz = core::PTZManager::getInstance();
            bool ok = ptz.relativeMove(camera_id, pan, tilt, zoom);
            if (!ok) return ApiUtils::createErrorResponse("PTZ relative move failed", 500, origin);
            return ApiUtils::createResponse(json::object(), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // GET /api/ptz/<cam_id>/presets — List presets
    CROW_ROUTE(app, "/api/ptz/<int>/presets")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return std::move(*err);

        auto& ptz = core::PTZManager::getInstance();
        auto presets = ptz.getPresets(camera_id);

        json arr = json::array();
        for (const auto& p : presets) {
            std::string token = std::to_string(p.id);
            arr.push_back({
                {"id", p.id},
                {"name", p.name},
                {"token", token},
                {"preset_token", token}
            });
        }
        return ApiUtils::createResponse({{"presets", arr}}, 200, origin);
    });

    // POST /api/ptz/<cam_id>/goto-preset — Go to preset
    CROW_ROUTE(app, "/api/ptz/<int>/goto-preset")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return std::move(*err);
        
        try {
            auto body = json::parse(req.body);
            auto preset_id = parsePresetIdentifier(body);
            if (!preset_id.has_value() || preset_id.value() < 0) {
                return ApiUtils::createErrorResponse("Missing preset_token or preset_id", 400, origin);
            }

            auto& ptz = core::PTZManager::getInstance();
            bool ok = ptz.gotoPreset(camera_id, preset_id.value());
            if (!ok) return ApiUtils::createErrorResponse("Failed to go to preset", 500, origin);
            return ApiUtils::createResponse({
                {"preset_token", std::to_string(preset_id.value())}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });


    // POST /api/ptz/<cam_id>/save-preset — Save preset
    CROW_ROUTE(app, "/api/ptz/<int>/save-preset")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            auto preset_id = parsePresetIdentifier(body);
            std::string name = body.value("name", "Preset");
            if (!preset_id.has_value() || preset_id.value() < 0) {
                return ApiUtils::createErrorResponse("Missing preset_token or preset_id", 400, origin);
            }

            auto& ptz = core::PTZManager::getInstance();
            bool ok = ptz.savePreset(camera_id, preset_id.value(), name);
            if (!ok) return ApiUtils::createErrorResponse("Failed to save preset", 500, origin);
            return ApiUtils::createResponse({
                {"preset_token", std::to_string(preset_id.value())}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // GET /api/ptz/<cam_id>/capabilities — Get PTZ capabilities
    CROW_ROUTE(app, "/api/ptz/<int>/capabilities")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return std::move(*err);

        auto& ptz = core::PTZManager::getInstance();
        auto caps = ptz.getCapabilities(camera_id);

        return ApiUtils::createResponse({
            {"can_pan", caps.can_pan},
            {"can_tilt", caps.can_tilt},
            {"can_zoom", caps.can_zoom},
            {"has_presets", caps.has_presets},
            {"max_presets", caps.max_presets}
        }, 200, origin);
    });

    LOG_INFO("[PTZController] Routes registered");
}

} // namespace api
} // namespace vms
