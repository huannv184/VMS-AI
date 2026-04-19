#include "api/hls_controller.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace vms {
namespace api {

void HLSController::registerRoutes(vms::server::VmsApp& app) {

    // GET /api/cameras/<id>/hls/stream.m3u8 — HLS master playlist
    CROW_ROUTE(app, "/api/cameras/<int>/hls/stream.m3u8")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) 
            return ApiUtils::createResponse(json::object(), 204, origin);

        std::string hls_dir = "hls/" + std::to_string(camera_id);
        std::string m3u8_path = hls_dir + "/stream.m3u8";

        if (!fs::exists(m3u8_path)) {
            return ApiUtils::createErrorResponse(
                "HLS stream not available for camera " + std::to_string(camera_id),
                404, origin);
        }

        // Read m3u8 file content
        std::ifstream file(m3u8_path);
        if (!file.is_open()) {
            return ApiUtils::createErrorResponse("Failed to read HLS playlist", 500, origin);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        crow::response res(200, content);
        res.set_header("Content-Type", "application/vnd.apple.mpegurl");
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        return res;
    });

    // GET /api/cameras/<id>/hls/<segment> — HLS .ts segments
    CROW_ROUTE(app, "/api/cameras/<int>/hls/<string>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int camera_id, std::string segment) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) 
            return ApiUtils::createResponse(json::object(), 204, origin);

        // Security: prevent path traversal
        if (segment.find("..") != std::string::npos || 
            segment.find('/') != std::string::npos ||
            segment.find('\\') != std::string::npos) {
            return ApiUtils::createErrorResponse("Invalid segment name", 400, origin);
        }

        // Only allow .ts and .m3u8 files
        if (segment.find(".ts") == std::string::npos && 
            segment.find(".m3u8") == std::string::npos) {
            return ApiUtils::createErrorResponse("Invalid file type", 400, origin);
        }

        std::string file_path = "hls/" + std::to_string(camera_id) + "/" + segment;
        
        if (!fs::exists(file_path)) {
            return ApiUtils::createErrorResponse("Segment not found", 404, origin);
        }

        // Read binary file
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            return ApiUtils::createErrorResponse("Failed to read segment", 500, origin);
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        std::string content_type = "video/MP2T";
        if (segment.find(".m3u8") != std::string::npos) {
            content_type = "application/vnd.apple.mpegurl";
        }

        crow::response res(200, content);
        res.set_header("Content-Type", content_type);
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
        res.set_header("Cache-Control", "no-cache");
        return res;
    });

    LOG_INFO("[HLSController] Routes registered");
}

} // namespace api
} // namespace vms
