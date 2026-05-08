// ==============================================================
// File: src/api/reid_controller.cpp
// Re-ID REST API — Gallery, Trail, Search, Config
// ==============================================================

#include "api/reid_controller.h"
#include "core/reid_engine.h"
#include "middleware/auth_middleware.h"
#include "server/vms_app.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#ifdef DELETE
#undef DELETE
#endif

using json = nlohmann::json;

namespace vms {
namespace api {

namespace {
// One-time WARN when REST is hit while the cross-camera ReID detection
// pipeline has no producer wired in. Without this signal, an operator looking
// at /api/reid/gallery → 200 [] cannot distinguish "scene currently empty"
// from "feature dead" — see BUG-REID-DEAD-PIPELINE 2026-05-08.
void warnIfProducerNotWiredOnce(const vms::core::ReIDEngine& engine) {
    if (engine.isProducerWired()) return;
    static std::once_flag warned;
    std::call_once(warned, []() {
        LOG_WARN("ReIDEngine: REST query received but no detection producer has called "
                 "processDetection() this process. Gallery/trail/search will stay empty. "
                 "Wire processDetection from the inference pipeline to enable cross-camera ReID.");
    });
}
} // namespace

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

    // GET /api/reid/trail/<int> — Cross-camera trail (REID_READ)
    CROW_ROUTE(app, "/api/reid/trail/<int>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int global_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::REID_READ, origin)) return std::move(*err);

        try {
            auto& engine = vms::core::ReIDEngine::getInstance();
            warnIfProducerNotWiredOnce(engine);
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

    // POST /api/reid/search — Search by image (REID_READ — read-only query)
    CROW_ROUTE(app, "/api/reid/search")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::REID_READ, origin)) return std::move(*err);

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
            // Guard against pathological top_k values from clients that could
            // request gigantic result vectors. The engine's gallery is bounded
            // (max_gallery_size, default 500) so anything beyond that is
            // pointless work — clamp here.
            if (top_k <= 0) top_k = 10;
            if (top_k > 1000) top_k = 1000;
            auto& engine = vms::core::ReIDEngine::getInstance();
            warnIfProducerNotWiredOnce(engine);
            auto results = engine.searchByImage(query, top_k);

            // Build response with entry details. The gallery copy is taken
            // once and indexed by global_id so the join is O(top_k + gallery)
            // instead of the previous O(top_k × gallery) inner-loop scan —
            // matters once max_gallery_size grows past the default 500.
            auto gallery = engine.getActiveGallery();
            std::unordered_map<int, const vms::core::ReIDEntry*> by_gid;
            by_gid.reserve(gallery.size());
            for (const auto& entry : gallery) by_gid.emplace(entry.global_id, &entry);

            json matches = json::array();
            for (const auto& [gid, score] : results) {
                json match = {{"global_id", gid}, {"score", score}};
                auto it = by_gid.find(gid);
                if (it != by_gid.end()) {
                    match["thumbnail_path"] = it->second->thumbnail_path;
                    match["camera_id"] = it->second->camera_id;
                }
                matches.push_back(match);
            }
            
            return ApiUtils::createResponse({
                {"results", matches},
                {"count", matches.size()},
                {"producer_wired", engine.isProducerWired()}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET/PUT /api/reid/config — GET=REID_READ, PUT=REID_WRITE
    CROW_ROUTE(app, "/api/reid/config")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::Put, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        const Permission needed = (req.method == crow::HTTPMethod::GET)
            ? Permission::REID_READ : Permission::REID_WRITE;
        if (auto err = ApiUtils::requirePermission(ctx, needed, origin)) return std::move(*err);

        try {
            auto& engine = vms::core::ReIDEngine::getInstance();

            if (req.method == crow::HTTPMethod::GET) {
                return ApiUtils::createResponse(engine.getConfig().toJSON(), 200, origin);
            }

            auto body = json::parse(req.body);
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

    // GET/DELETE /api/reid/gallery — GET=REID_READ, DELETE=REID_WRITE
    CROW_ROUTE(app, "/api/reid/gallery")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::Delete, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        const Permission needed = (req.method == crow::HTTPMethod::GET)
            ? Permission::REID_READ : Permission::REID_WRITE;
        if (auto err = ApiUtils::requirePermission(ctx, needed, origin)) return std::move(*err);

        auto& engine = vms::core::ReIDEngine::getInstance();
        if (req.method == crow::HTTPMethod::GET) {
            warnIfProducerNotWiredOnce(engine);
            auto gallery = engine.getActiveGallery();

            json result = json::array();
            for (const auto& entry : gallery) {
                result.push_back(entry.toJSON());
            }

            return ApiUtils::createResponse({
                {"gallery", result},
                {"count", result.size()},
                {"producer_wired", engine.isProducerWired()},
                {"statistics", engine.getStatistics()}
            }, 200, origin);
        }

        engine.clearGallery();
        return ApiUtils::createResponse(json::object(), 200, origin);
    });

    // GET /api/reid/statistics (REID_READ)
    CROW_ROUTE(app, "/api/reid/statistics")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::REID_READ, origin)) return std::move(*err);

        auto& engine = vms::core::ReIDEngine::getInstance();
        warnIfProducerNotWiredOnce(engine);
        return ApiUtils::createResponse(engine.getStatistics(), 200, origin);
    });

    LOG_INFO("Re-ID routes registered");
}

} // namespace api
} // namespace vms
