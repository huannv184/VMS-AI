#include "api/snapshot_controller.h"
#include "core/snapshot_manager.h"
#include "utils/api_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>

using json = nlohmann::json;

namespace vms {
namespace api {

void SnapshotController::registerRoutes(vms::server::VmsApp& app) {
    // GET /api/snapshots
    CROW_ROUTE(app, "/api/snapshots")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& manager = vms::core::SnapshotManager::getInstance();
        int camera_id = req.url_params.get("camera_id") ? std::atoi(req.url_params.get("camera_id")) : -1;
        int limit = req.url_params.get("limit") ? std::atoi(req.url_params.get("limit")) : 50;
        
        auto snapshots = manager.getRecentSnapshots(camera_id, limit);
        json out = json::array();
        for (const auto& s : snapshots) {
            out.push_back({
                {"id", s.id},
                {"camera_id", s.camera_id},
                {"trigger", s.trigger},
                {"filepath", s.filepath},
                {"timestamp", s.timestamp_str},
                {"metadata", json::parse(s.metadata_json)}
            });
        }
        return ApiUtils::createResponse({{"snapshots", out}}, 200, origin);
    });

    // GET/DELETE /api/snapshots/:id
    CROW_ROUTE(app, "/api/snapshots/<string>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([](const crow::request& req, const std::string& id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& manager = vms::core::SnapshotManager::getInstance();
        
        if (req.method == crow::HTTPMethod::Get) {
            auto snaps = manager.getRecentSnapshots();
            auto it = std::find_if(snaps.begin(), snaps.end(), [&](const vms::Snapshot& s) { return s.id == id; });
            if (it == snaps.end()) return ApiUtils::createErrorResponse("Not found", 404, origin);
            
            return ApiUtils::createResponse({
                {"id", it->id},
                {"camera_id", it->camera_id},
                {"trigger", it->trigger},
                {"filepath", it->filepath},
                {"timestamp", it->timestamp_str},
                {"metadata", json::parse(it->metadata_json)}
            }, 200, origin);
        }

        if (manager.deleteSnapshot(id)) {
            return ApiUtils::createResponse(json::object(), 200, origin);
        }
        return ApiUtils::createErrorResponse("Snapshot not found or failed to delete", 404, origin);
    });

    // GET /api/snapshots/files/:filename
    CROW_ROUTE(app, "/api/snapshots/files/<string>")
    .methods(crow::HTTPMethod::Get)
    ([](const crow::request&, const std::string& filename) {
        if (filename.find("..") != std::string::npos ||
            filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos) {
            return crow::response(400, "Invalid filename");
        }

        std::string filepath = "snapshots/" + filename;
        if (!std::filesystem::exists(filepath)) {
            return crow::response(404, "Snapshot not found");
        }

        crow::response res;
        res.code = 200;
        res.set_static_file_info(filepath);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "public, max-age=86400");
        
        // Set Content-Type based on extension
        if (filename.find(".jpg") != std::string::npos || filename.find(".jpeg") != std::string::npos) {
            res.set_header("Content-Type", "image/jpeg");
        } else if (filename.find(".png") != std::string::npos) {
            res.set_header("Content-Type", "image/png");
        } else {
            res.set_header("Content-Type", "application/octet-stream");
        }
        return res;
    });
}

} // namespace api
} // namespace vms
