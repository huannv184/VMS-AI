#include "api/camera_discovery_controller.h"
#include "core/network_scanner.h"
#include "core/camera_discovery.hpp"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include "server/vms_app.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace vms {
namespace api {

// PENDING-AUDIT-2026-05-09: 3 empty-capture handlers (discover POST, status GET, results GET).
// Discovery POST can scan internal LAN without auth (port-scan amplification).
// Tracked in past-bugs.md → BUG-LINT-CONTROLLERS-PENDING-2026-05-09.
// LINT-ALLOW-NO-AUTH: PENDING-AUDIT-2026-05-09 (discover POST)
// LINT-ALLOW-NO-AUTH: PENDING-AUDIT-2026-05-09 (discover status by id)
// LINT-ALLOW-NO-AUTH: PENDING-AUDIT-2026-05-09 (discover results by id)
void CameraDiscoveryController::registerRoutes(vms::server::VmsApp& app) {

    // ═══════════════════════════════════════════════════════════════
    //  POST /api/cameras/discover
    //  Direct brand probe & stream list (fast, 3-5 seconds)
    //  Called by frontend "KIỂM TRA" button
    // ═══════════════════════════════════════════════════════════════
    CROW_ROUTE(app, "/api/cameras/discover")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req) {
        std::string origin = vms::api::ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) {
            return vms::api::ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            auto body = json::parse(req.body);

            // Extract connection parameters from frontend
            std::string host     = body.value("host", "");
            std::string username = body.value("username", "admin");
            std::string password = body.value("password", "");
            int http_port        = body.value("http_port", 80);
            int rtsp_port        = body.value("rtsp_port", 554);

            if (host.empty()) {
                return vms::api::ApiUtils::createErrorResponse(
                    "Thiếu tham số 'host' (địa chỉ IP camera)", 400, origin);
            }

            LOG_INFO("=== CAMERA BRAND DETECTION ===");
            LOG_INFO("Host: {}:{}, User: {}", host, http_port, username);

            // ── Build DiscoveryConfig ──────────────────────────
            CameraDiscovery::DiscoveryConfig cfg;
            cfg.host       = host;
            cfg.username   = username;
            cfg.password   = password;
            cfg.http_port  = http_port;
            cfg.rtsp_port  = rtsp_port;
            cfg.timeout_sec = 5;
            cfg.brand      = CameraDiscovery::Brand::Unknown; // Let AutoDetector probe

            // ── Probe with progress callback ──────────────────
            json probe_log = json::array();

            auto result = CameraDiscovery::AutoDetector::discover(cfg,
                [&probe_log](const std::string& brand_name, bool hit) {
                    LOG_INFO("Probe [{}]: {}", brand_name, hit ? "MATCH" : "miss");
                    probe_log.push_back({
                        {"brand", brand_name},
                        {"match", hit}
                    });
                }
            );

            // ── Build response ────────────────────────────────
            json response;
            std::string brand_str = CameraDiscovery::brandToString(result.brand);

            response["brand"]    = brand_str;
            response["model"]    = result.model;
            response["serial"]   = result.serial;
            response["firmware"] = result.firmware;
            response["channels"] = result.channels;

            // Camera/stream list
            json cameras_json = json::array();
            for (const auto& cam : result.cameras) {
                cameras_json.push_back({
                    {"channel_id", cam.channel_id},
                    {"name",       cam.name},
                    {"rtsp_main",  cam.rtsp_main},
                    {"rtsp_sub",   cam.rtsp_sub},
                    {"resolution", cam.resolution},
                    {"codec",      cam.codec},
                    {"online",     cam.online}
                });
            }
            response["cameras"]   = cameras_json;
            response["probe_log"] = probe_log;

            // If detection failed entirely
            if (!result.error.empty()) {
                response["error"] = result.error;
                LOG_WARN("Brand detection failed for {}: {}", host, result.error);
                return vms::api::ApiUtils::createResponse(response, 200, origin);
            }

            LOG_INFO("=== DETECTION RESULT: {} | Model: {} | Channels: {} ===",
                     brand_str, result.model, result.channels);

            return vms::api::ApiUtils::createResponse(response, 200, origin);

        } catch (const json::exception& e) {
            LOG_ERROR("JSON parse error in /api/cameras/discover: {}", e.what());
            return vms::api::ApiUtils::createErrorResponse(
                "Lỗi parse JSON: " + std::string(e.what()), 400, origin);
        } catch (const std::exception& e) {
            LOG_ERROR("Error in /api/cameras/discover: {}", e.what());
            return vms::api::ApiUtils::createErrorResponse(
                "Lỗi server: " + std::string(e.what()), 500, origin);
        }
    });

    // ═══════════════════════════════════════════════════════════════
    //  Network Scan routes (kept for Deep Scan feature)
    // ═══════════════════════════════════════════════════════════════

    CROW_ROUTE(app, "/api/cameras/discover/status/<string>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, std::string scan_id) {
        std::string origin = vms::api::ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) return vms::api::ApiUtils::createResponse(json::object(), 204, origin);

        auto& scanner = core::NetworkScanner::getInstance();
        return vms::api::ApiUtils::createResponse(scanner.getScanStatus(scan_id), 200, origin);
    });

    CROW_ROUTE(app, "/api/cameras/discover/results/<string>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, std::string scan_id) {
        std::string origin = vms::api::ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) return vms::api::ApiUtils::createResponse(json::object(), 204, origin);

        auto& scanner = core::NetworkScanner::getInstance();
        return vms::api::ApiUtils::createResponse(scanner.getScanResults(scan_id), 200, origin);
    });
}

} // namespace api
} // namespace vms
