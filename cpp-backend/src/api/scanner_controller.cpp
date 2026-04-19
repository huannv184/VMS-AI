#include "api/scanner_controller.h"
#include "core/network_scanner.h"
#include "utils/api_utils.h"
#include "server/vms_app.h"

namespace vms {
namespace api {

void ScannerController::registerRoutes(vms::server::VmsApp& app) {

    // Start Scan
    CROW_ROUTE(app, "/api/scan/start")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(nlohmann::json::object(), 204, origin);
        
        try {
            auto body = nlohmann::json::parse(req.body);
            core::ScanConfig config;
            
            if (body.contains("network_range")) {
                config.network_range = body["network_range"];
            } else {
                return ApiUtils::createErrorResponse("network_range is required", 400, origin);
            }
            if (body.contains("enable_brute_force")) {
                config.enable_brute_force = body["enable_brute_force"];
            }
            if (body.contains("custom_ports") && body["custom_ports"].is_array()) {
                for (auto& p : body["custom_ports"]) {
                    config.custom_ports.push_back(p);
                }
            }

            auto& scanner = core::NetworkScanner::getInstance();
            std::string scan_id = scanner.startScan(config);

            return ApiUtils::createResponse({
                {"scan_id", scan_id},
                {"status", "started"}
            }, 200, origin);

        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // Get Scan Status
    CROW_ROUTE(app, "/api/scan/status/<string>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req, std::string scan_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(nlohmann::json::object(), 204, origin);

        auto& scanner = core::NetworkScanner::getInstance();
        nlohmann::json result = scanner.getScanStatus(scan_id);

        if (result.contains("error")) {
            return ApiUtils::createErrorResponse("Not found", 404, origin);
        }

        return ApiUtils::createResponse(result, 200, origin);
    });

    // Get Scan Results
    CROW_ROUTE(app, "/api/scan/results/<string>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req, std::string scan_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(nlohmann::json::object(), 204, origin);

        auto& scanner = core::NetworkScanner::getInstance();
        nlohmann::json result = scanner.getScanResults(scan_id);

        if (result.contains("error")) {
            return ApiUtils::createErrorResponse("Not found", 404, origin);
        }

        return ApiUtils::createResponse(result, 200, origin);
    });
}

} // namespace api
} // namespace vms
