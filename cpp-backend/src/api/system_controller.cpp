#include "api/system_controller.h"
#include "utils/system_stats.h"
#include "utils/api_utils.h"
#include "core/onvif_discovery.h"
#include "database/db_manager.h"
#include "database/db_state.h"
#include "database/audit_repository.h"
#include "database/user_repository.h"
#include "core/brands/core_factory.hpp"
#include "utils/sha256.h"
#include "utils/config.h"
#include "utils/validator.h"
#include "middleware/auth_middleware.h"
#include "core/network_scanner.h"
#include "streaming/camera_stream_manager_qt.h"
#include "core/camera_pipeline_manager.h"
#include <unordered_set>

using json = nlohmann::json;

namespace vms {
namespace api {

namespace {

bool isBooleanKey(const std::string& key) {
    static const std::unordered_set<std::string> keys = {
        "storage_autoDelete", "storage_cloudBackup", "net_ssl", "ai_yolo", "ai_lpr",
        "ai_face", "ai_behavior", "ai_gpu", "ppe_helmet", "ppe_vest", "ppe_glasses",
        "ppe_mask", "alert_email_enabled", "alert_sms_enabled", "alert_sound",
        "api_enabled", "backup_nas", "backup_cloud", "analytics_cleanup_enabled"
    };
    return keys.find(key) != keys.end();
}

bool isIntKey(const std::string& key) {
    return key == "net_rtsp_port" || key == "net_web_port" || key == "analytics_retention_days";
}

std::optional<std::string> normalizeSettingValue(const std::string& key, const json& value) {
    if (isBooleanKey(key)) {
        if (!value.is_boolean()) return std::nullopt;
        return value.get<bool>() ? "true" : "false";
    }

    if (isIntKey(key)) {
        int val = -1;
        if (value.is_number_integer()) {
            val = value.get<int>();
        } else if (value.is_string()) {
            try {
                val = std::stoi(value.get<std::string>());
            } catch (...) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
        return std::to_string(val);
    }

    if (value.is_object() || value.is_array()) return value.dump();
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number()) return value.dump();
    return std::nullopt;
}

json parseStoredSettingValue(const std::string& key, const std::string& value) {
    if (isBooleanKey(key)) {
        return value == "true" || value == "1";
    }

    if (isIntKey(key)) {
        try {
            return std::stoi(value);
        } catch (...) {
            return value;
        }
    }

    if (!value.empty() && ((value.front() == '{' && value.back() == '}') || (value.front() == '[' && value.back() == ']'))) {
        try {
            return json::parse(value);
        } catch (...) {
            return value;
        }
    }

    return value;
}

}

void SystemController::registerRoutes(vms::server::VmsApp& app) {

    // GET /api/health
    // ISOLATION GUARANTEE: Always returns 200. No DB access. No auth.
    // No blocking calls. No mutex. Reads only atomic flags (O(1), lock-free).
    CROW_ROUTE(app, "/api/health")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        // Dependency status is INFORMATIONAL — health always returns 200.
        // This enables load balancers and monitoring to distinguish between
        // "backend alive" vs "backend alive + fully operational".
        bool db_ok = vms::database::db_ready.load(std::memory_order_relaxed);

        crow::response res;
        res.code = 200;
        res.set_header("Content-Type", "application/json");
        ApiUtils::applyCors(res, origin);
        res.body = db_ok
            ? R"({"success":true,"data":{"status":"ok","database":"connected"},"error":null})"
            : R"({"success":true,"data":{"status":"degraded","database":"unavailable"},"error":null})";
        return res;
    });

    // GET /api/system/stats
    CROW_ROUTE(app, "/api/system/stats")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
            auto m = utils::SystemStats::getMetrics();
            
            json j;
            j["cpu_percent"] = m.cpu_usage_percent;
            j["ram_total_gb"] = m.ram_total_gb;
            j["ram_used_gb"] = m.ram_used_gb;
            j["ram_percent"] = m.ram_usage_percent;
            j["disk_total_gb"] = m.disk_total_gb;
            j["disk_used_gb"] = m.disk_used_gb;
            j["disk_percent"] = m.disk_usage_percent;
            j["gpu_percent"] = m.gpu_usage_percent;
            j["gpu_memory_used_mb"] = m.gpu_memory_used_mb;
            j["gpu_memory_total_mb"] = m.gpu_memory_total_mb;
            j["uptime_seconds"] = m.uptime_seconds;
            j["cpu_model"] = m.cpu_model;
            j["ai_accuracy"] = m.ai_accuracy;
            
            return ApiUtils::createResponse(j, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET /api/system/onvif/discover
    CROW_ROUTE(app, "/api/system/onvif/discover")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
            auto devices = vms::core::OnvifDiscovery::discover();
            json result = json::array();
            for (const auto& dev : devices) {
                CameraDiscovery::Brand brand_id = CameraDiscovery::Brand::Unknown;
                if (dev.manufacturer == "Hikvision") brand_id = CameraDiscovery::Brand::Hikvision;
                else if (dev.manufacturer == "Dahua") brand_id = CameraDiscovery::Brand::Dahua;
                else if (dev.manufacturer == "Axis") brand_id = CameraDiscovery::Brand::Axis;
                else if (dev.manufacturer == "Bosch") brand_id = CameraDiscovery::Brand::Bosch;
                else if (dev.manufacturer == "Hanwha") brand_id = CameraDiscovery::Brand::Hanwha;
                else if (dev.manufacturer == "Uniview") brand_id = CameraDiscovery::Brand::Uniview;
                else if (dev.manufacturer == "Reolink") brand_id = CameraDiscovery::Brand::Reolink;
                else if (dev.manufacturer == "Milesight") brand_id = CameraDiscovery::Brand::Milesight;
                else if (dev.manufacturer == "KBVision") brand_id = CameraDiscovery::Brand::Kbvision;
                else if (dev.manufacturer == "Imou") brand_id = CameraDiscovery::Brand::Imou;
                else if (dev.manufacturer == "Ezviz") brand_id = CameraDiscovery::Brand::Ezviz;
                else if (dev.manufacturer == "Generic ONVIF") brand_id = CameraDiscovery::Brand::ONVIF;

                std::string rtsp_path = "/stream";
                if (brand_id == CameraDiscovery::Brand::Hikvision) rtsp_path = "/Streaming/Channels/101";
                else if (brand_id == CameraDiscovery::Brand::Dahua) rtsp_path = "/cam/realmonitor?channel=1&subtype=0";
                else if (brand_id == CameraDiscovery::Brand::Uniview) rtsp_path = "/video1";
                else if (brand_id == CameraDiscovery::Brand::Hanwha) rtsp_path = "/profile1/media.smp";
                else if (brand_id == CameraDiscovery::Brand::Reolink) rtsp_path = "/h264Preview_01_main";

                result.push_back({
                    {"ip", dev.ip},
                    {"service_url", dev.service_url},
                    {"name", dev.name},
                    {"manufacturer", dev.manufacturer},
                    {"model", dev.model},
                    {"brand_id", (int)brand_id},
                    {"recommended_rtsp_path", rtsp_path}
                });
            }
            return ApiUtils::createResponse({{"devices", result}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET/PUT /api/system/settings
    CROW_ROUTE(app, "/api/system/settings")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) {
            return ApiUtils::createErrorResponse("Admin privileges required", 403, origin);
        }

        if (req.method == crow::HTTPMethod::Get) {
            auto& db = vms::database::DbManager::getInstance();
            json result = json::object();
            auto settings_map = db.getAllSettings();
            
            for (const auto& [k, v] : settings_map) {
                result[k] = parseStoredSettingValue(k, v);
            }
            
            return ApiUtils::createResponse(result, 200, origin);
        }

        if (req.method == crow::HTTPMethod::Put) {
            try {
                auto body = json::parse(req.body);
                auto& db = vms::database::DbManager::getInstance();
                
                for (auto& el : body.items()) {
                    std::string key = el.key();
                    auto value = normalizeSettingValue(key, el.value());
                    if (!value.has_value()) {
                        return ApiUtils::createErrorResponse("Invalid setting value for key: " + key, 400, origin);
                    }
                    db.setSetting(key, value.value());
                }
                
                return ApiUtils::createResponse(json::object(), 200, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // GET /api/audit-logs
    CROW_ROUTE(app, "/api/audit-logs")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        {
            auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
            if (!ctx.user.has_value()) {
                return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
            }
            if (ctx.user->role_id != 1) {
                return ApiUtils::createErrorResponse("Admin privileges required", 403, origin);
            }
        }

        try {
            int limit = 50;
            int offset = 0;
            if (req.url_params.get("limit")) limit = std::stoi(req.url_params.get("limit"));
            if (req.url_params.get("offset")) offset = std::stoi(req.url_params.get("offset"));

            vms::database::AuditRepository repo;
            auto logs = repo.getLogs(limit, offset);
            json result = json::array();
            for (const auto& log : logs) {
                result.push_back(log.toJson());
            }
            return ApiUtils::createResponse({{"logs", result}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // POST /api/system/network/scan/start
    CROW_ROUTE(app, "/api/system/network/scan/start")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) {
            return ApiUtils::createErrorResponse("Admin privileges required", 403, origin);
        }

        try {
            vms::core::ScanConfig cfg;
            cfg.network_range = "192.168.1.0/24";
            
            if (!req.body.empty()) {
                auto body = json::parse(req.body);
                if (body.contains("network_range") && body["network_range"].is_string()) {
                    cfg.network_range = body["network_range"].get<std::string>();
                }
                if (body.contains("enable_brute_force") && body["enable_brute_force"].is_boolean()) {
                    cfg.enable_brute_force = body["enable_brute_force"].get<bool>();
                }
            }
            
            std::string scan_id = vms::core::NetworkScanner::getInstance().startScan(cfg);
            return ApiUtils::createResponse({{"scan_id", scan_id}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET /api/system/network/scan/status
    CROW_ROUTE(app, "/api/system/network/scan/status")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) {
            return ApiUtils::createErrorResponse("Admin privileges required", 403, origin);
        }

        try {
            std::string scan_id = req.url_params.get("scan_id") ? req.url_params.get("scan_id") : "";
            if (scan_id.empty()) {
                return ApiUtils::createErrorResponse("Missing scan_id", 400, origin);
            }
            
            auto& scanner = vms::core::NetworkScanner::getInstance();
            auto status_j = scanner.getScanStatus(scan_id);
            if (status_j.contains("error")) {
                return ApiUtils::createErrorResponse(status_j["error"].get<std::string>(), 404, origin);
            }
            
            auto results_j = scanner.getScanResults(scan_id);
            if (results_j.contains("results")) {
                status_j["results"] = results_j["results"];
            } else {
                status_j["results"] = json::array();
            }
            
            return ApiUtils::createResponse(status_j, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // POST /api/system/network/scan/stop
    CROW_ROUTE(app, "/api/system/network/scan/stop")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) {
            return ApiUtils::createErrorResponse("Admin privileges required", 403, origin);
        }

        try {
            auto body = json::parse(req.body);
            std::string scan_id = body.value("scan_id", "");
            if (scan_id.empty()) {
                return ApiUtils::createErrorResponse("Missing scan_id field", 400, origin);
            }
            
            vms::core::NetworkScanner::getInstance().stopScan(scan_id);
            return ApiUtils::createResponse(json::object(), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET/DELETE /api/system/network/cache
    CROW_ROUTE(app, "/api/system/network/cache")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) {
            return ApiUtils::createErrorResponse("Admin privileges required", 403, origin);
        }

        auto& scanner = vms::core::NetworkScanner::getInstance();

        if (req.method == crow::HTTPMethod::Delete) {
            scanner.clearCache();
            return ApiUtils::createResponse(json::object(), 200, origin);
        }

        return ApiUtils::createResponse(scanner.getCachedDevicesJson(), 200, origin);
    });

    // GET /api/system/streaming-config
    // Returns the actual WebSocket port (may differ from config if port was in use).
    // Frontend must call this on startup and connect to the returned port.
    CROW_ROUTE(app, "/api/system/streaming-config")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        int ws_port = vms::streaming::CameraStreamManager::getInstance().getBoundPort();
        json result;
        result["websocket_port"] = ws_port;
        result["websocket_available"] = (ws_port > 0);
        return ApiUtils::createResponse(result, 200, origin);
    });

}

} // namespace api
} // namespace vms
