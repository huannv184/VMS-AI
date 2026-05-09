#include "api/snapshot_controller.h"
#include "core/snapshot_manager.h"
#include "database/audit_repository.h"
#include "middleware/auth_middleware.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>

using json = nlohmann::json;

namespace vms {
namespace api {

// BUG-SNAP-AUTH-01 (audit 2026-05-09): pre-fix all 3 snapshot routes used
// `[]` capture and never read AuthMiddleware context. Anyone reachable on
// the API port could (a) list every snapshot in the system (camera_id +
// filepath + metadata = sensitive surveillance PII), (b) read raw image
// files via /api/snapshots/files/<filename>, (c) DELETE individual
// snapshots with no auth, no audit log. Same SEC-shape as BUG-ANPR-AUTH-01
// (5 routes, 2026-05-08), SEC-008/009 (attendance/counter unauth GETs).
// Fix: GET = RECORDING_READ (snapshots are evidence/PII), DELETE =
// RECORDING_DELETE, file fetch = RECORDING_READ. Audit log on DELETE.

void SnapshotController::registerRoutes(vms::server::VmsApp& app) {
    // GET /api/snapshots
    CROW_ROUTE(app, "/api/snapshots")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::RECORDING_READ, origin)) return std::move(*err);

        auto& manager = vms::core::SnapshotManager::getInstance();
        int camera_id = req.url_params.get("camera_id") ? std::atoi(req.url_params.get("camera_id")) : -1;
        int limit = req.url_params.get("limit") ? std::atoi(req.url_params.get("limit")) : 50;
        // Clamp pathological values — pre-fix a client could request limit=10M
        // and the snapshot manager would blindly try to materialise that many
        // rows. Bound to a reasonable page size.
        if (limit < 1) limit = 50;
        if (limit > 500) limit = 500;
        
        auto snapshots = manager.getRecentSnapshots(camera_id, limit);
        json out = json::array();
        for (const auto& s : snapshots) {
            // BUG-M2 FIX: a single corrupt metadata_json row used to throw
            // out of json::parse and 500 the whole list endpoint. Tolerate
            // bad rows by returning an empty metadata object for them.
            json meta = json::object();
            try {
                if (!s.metadata_json.empty()) meta = json::parse(s.metadata_json);
            } catch (...) {}
            out.push_back({
                {"id", s.id},
                {"camera_id", s.camera_id},
                {"trigger", s.trigger},
                {"filepath", s.filepath},
                {"timestamp", s.timestamp_str},
                {"metadata", meta}
            });
        }
        return ApiUtils::createResponse({{"snapshots", out}}, 200, origin);
    });

    // GET/DELETE /api/snapshots/:id
    CROW_ROUTE(app, "/api/snapshots/<string>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, const std::string& id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        const Permission needed = (req.method == crow::HTTPMethod::Get)
            ? Permission::RECORDING_READ : Permission::RECORDING_DELETE;
        if (auto err = ApiUtils::requirePermission(ctx, needed, origin)) return std::move(*err);

        auto& manager = vms::core::SnapshotManager::getInstance();

        if (req.method == crow::HTTPMethod::Get) {
            auto snaps = manager.getRecentSnapshots();
            auto it = std::find_if(snaps.begin(), snaps.end(), [&](const vms::Snapshot& s) { return s.id == id; });
            if (it == snaps.end()) return ApiUtils::createErrorResponse("Not found", 404, origin);
            
            json meta = json::object();
            try {
                if (!it->metadata_json.empty()) meta = json::parse(it->metadata_json);
            } catch (...) {}
            return ApiUtils::createResponse({
                {"id", it->id},
                {"camera_id", it->camera_id},
                {"trigger", it->trigger},
                {"filepath", it->filepath},
                {"timestamp", it->timestamp_str},
                {"metadata", meta}
            }, 200, origin);
        }

        if (manager.deleteSnapshot(id)) {
            // BUG-SNAP-AUDIT-01: snapshots are evidence material; deletion
            // must be traceable to a specific user. Pre-fix the only record
            // was the implicit `now-it's-gone` change in DB.
            if (ctx.user) {
                vms::database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "DELETE_SNAPSHOT", "Snapshot id=" + id);
            }
            return ApiUtils::createResponse(json::object(), 200, origin);
        }
        return ApiUtils::createErrorResponse("Snapshot not found or failed to delete", 404, origin);
    });

    // GET /api/snapshots/files/:filename
    CROW_ROUTE(app, "/api/snapshots/files/<string>")
    .methods(crow::HTTPMethod::Get)
    ([&app](const crow::request& req, const std::string& filename) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::RECORDING_READ, origin)) {
            // requirePermission returns a Crow response; convert to bare 401/403
            // so the file-serving callers get a uniform error rather than a
            // CORS-padded JSON body where they expect raw image bytes.
            return std::move(*err);
        }
        if (filename.find("..") != std::string::npos ||
            filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos) {
            return crow::response(400, "Invalid filename");
        }

        std::string filepath = "data/snapshots/" + filename;
        if (!std::filesystem::exists(filepath)) {
            return crow::response(404, "Snapshot not found");
        }

        crow::response res;
        res.code = 200;
        res.set_static_file_info(filepath);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "public, max-age=86400");

        // Set Content-Type based on extension SUFFIX — not substring. The
        // previous `find(".jpg")` matched anywhere, so a file named
        // `payload.jpg.exe` would be served as image/jpeg and could be
        // sniffed as an executable by older browsers.
        auto endsWithCi = [&](const std::string& suffix) {
            if (filename.size() < suffix.size()) return false;
            for (size_t i = 0; i < suffix.size(); ++i) {
                char a = filename[filename.size() - suffix.size() + i];
                char b = suffix[i];
                if (std::tolower(static_cast<unsigned char>(a)) !=
                    std::tolower(static_cast<unsigned char>(b))) return false;
            }
            return true;
        };

        if (endsWithCi(".jpg") || endsWithCi(".jpeg")) {
            res.set_header("Content-Type", "image/jpeg");
        } else if (endsWithCi(".png")) {
            res.set_header("Content-Type", "image/png");
        } else {
            res.set_header("Content-Type", "application/octet-stream");
        }
        return res;
    });
}

} // namespace api
} // namespace vms
