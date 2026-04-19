// ==============================================================
// File: src/api/reid_controller.cpp
// Re-ID REST API — Gallery, Trail, Search, Config
// ==============================================================

#include "api/reid_controller.h"
#include "core/reid_engine.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#ifdef DELETE
#undef DELETE
#endif

using json = nlohmann::json;

namespace vms {
namespace api {

// Base64 decode helper
static std::vector<unsigned char> base64Decode(const std::string& encoded) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> result;
    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        auto pos = chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + (int)pos;
        valb += 6;
        if (valb >= 0) {
            result.push_back((unsigned char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

void ReIDController::registerRoutes(vms::server::VmsApp& app) {
    LOG_INFO("Registering Re-ID routes...");

    // GET /api/reid/gallery — Active gallery
    CROW_ROUTE(app, "/api/reid/gallery")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto& engine = vms::core::ReIDEngine::getInstance();
            auto gallery = engine.getActiveGallery();
            
            json result = json::array();
            for (const auto& entry : gallery) {
                result.push_back(entry.toJSON());
            }
            
            return ApiUtils::createResponse({
                {"gallery", result},
                {"count", result.size()},
                {"statistics", engine.getStatistics()}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET /api/reid/trail/<int> — Cross-camera trail
    CROW_ROUTE(app, "/api/reid/trail/<int>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int global_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto& engine = vms::core::ReIDEngine::getInstance();
            auto trail = engine.getPersonTrail(global_id);
            
            json points = json::array();
            for (const auto& pt : trail) {
                points.push_back(pt.toJSON());
            }
            
            return ApiUtils::createResponse({
                {"global_id", global_id},
                {"trail", points},
                {"cameras_visited", points.size()}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // POST /api/reid/search — Search by image
    CROW_ROUTE(app, "/api/reid/search")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto body = json::parse(req.body);
            
            if (!body.contains("image")) {
                return ApiUtils::createErrorResponse("Missing 'image' (base64)", 400, origin);
            }
            
            std::string b64 = body["image"].get<std::string>();
            // Remove data URI prefix if present
            auto comma_pos = b64.find(',');
            if (comma_pos != std::string::npos) b64 = b64.substr(comma_pos + 1);
            
            auto decoded = base64Decode(b64);
            cv::Mat query = cv::imdecode(decoded, cv::IMREAD_COLOR);
            
            if (query.empty()) {
                return ApiUtils::createErrorResponse("Invalid image data", 400, origin);
            }
            
            int top_k = body.value("top_k", 10);
            auto& engine = vms::core::ReIDEngine::getInstance();
            auto results = engine.searchByImage(query, top_k);
            
            // Build response with entry details
            auto gallery = engine.getActiveGallery();
            json matches = json::array();
            for (const auto& [gid, score] : results) {
                json match = {{"global_id", gid}, {"score", score}};
                for (const auto& entry : gallery) {
                    if (entry.global_id == gid) {
                        match["thumbnail_path"] = entry.thumbnail_path;
                        match["camera_id"] = entry.camera_id;
                        break;
                    }
                }
                matches.push_back(match);
            }
            
            return ApiUtils::createResponse({
                {"results", matches},
                {"count", matches.size()}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET /api/reid/config
    CROW_ROUTE(app, "/api/reid/config")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& engine = vms::core::ReIDEngine::getInstance();
        return ApiUtils::createResponse(engine.getConfig().toJSON(), 200, origin);
    });

    // PUT /api/reid/config
    CROW_ROUTE(app, "/api/reid/config")
    .methods(crow::HTTPMethod::Put)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto body = json::parse(req.body);
            auto& engine = vms::core::ReIDEngine::getInstance();
            
            auto config = engine.getConfig();
            if (body.contains("enabled")) config.enabled = body["enabled"].get<bool>();
            if (body.contains("match_threshold")) {
                float t = body["match_threshold"].get<float>();
                if (t < 0.0f || t > 1.0f) return ApiUtils::createErrorResponse("threshold must be 0-1", 400, origin);
                config.match_threshold = t;
            }
            if (body.contains("gallery_ttl_sec")) config.gallery_ttl_sec = body["gallery_ttl_sec"].get<int>();
            if (body.contains("max_gallery_size")) config.max_gallery_size = body["max_gallery_size"].get<int>();
            
            engine.setConfig(config);
            return ApiUtils::createResponse({{"config", config.toJSON()}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // DELETE /api/reid/gallery — Clear gallery
    CROW_ROUTE(app, "/api/reid/gallery")
    .methods(crow::HTTPMethod::Delete)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& engine = vms::core::ReIDEngine::getInstance();
        engine.clearGallery();
        return ApiUtils::createResponse(json::object(), 200, origin);
    });

    // GET /api/reid/statistics
    CROW_ROUTE(app, "/api/reid/statistics")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& engine = vms::core::ReIDEngine::getInstance();
        return ApiUtils::createResponse(engine.getStatistics(), 200, origin);
    });

    LOG_INFO("Re-ID routes registered");
}

} // namespace api
} // namespace vms
