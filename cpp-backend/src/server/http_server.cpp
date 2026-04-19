#include "server/http_server.h"
#include "api/camera_controller.h"
#include "api/roi_controller.h"
#include "api/event_controller.h"
#include "api/recording_controller.h"
#include "api/tracking_controller.h"
#include "api/face_controller.h"
#include "api/anpr_controller.h"
#include "api/user_controller.h"
#include "api/system_controller.h"
#include "api/synopsis_controller.h"
#include "api/export_controller.h"
#include "api/snapshot_controller.h"
#include "api/traffic_controller.h"
#include "api/camera_discovery_controller.h"
#include "api/scanner_controller.h"
#include "api/event_engine_controller.h"
#include "api/ptz_controller.h"
#include "api/hls_controller.h"
#include "api/device_controller.h"
#include "api/site_controller.h"
#include "api/analytics_controller.h"
#include "api/reporting_controller.h"
#include "api/videowall_controller.h"
#include "api/reid_controller.h"
#include "api/alert_controller.h"
#include "middleware/auth_middleware.h"
#include "utils/logger.h"
#include "utils/api_utils.h"
#include "core/analytics_cleanup_service.h"
#include "streaming/camera_stream_manager_qt.h"

#include <filesystem>

namespace vms {
namespace server {

namespace {

std::string resolve_frontend_dist_dir() {
    static std::string cached;
    static bool once = false;
    if (once) {
        return cached;
    }
    once = true;
    static const std::vector<std::string> candidates = {
        "vms-frontend/dist",
        "../vms-frontend/dist",
        "../../vms-frontend/dist",
        "../../../vms-frontend/dist",
        "dist",
        "../dist",
    };
    namespace fs = std::filesystem;
    for (const auto& c : candidates) {
        std::error_code ec;
        const fs::path p = fs::path(c) / "index.html";
        if (fs::is_regular_file(p, ec)) {
            cached = c;
            LOG_INFO("Frontend bundle: using {}", fs::weakly_canonical(fs::path(c), ec).string());
            return cached;
        }
    }
    cached = "../../../vms-frontend/dist";
    LOG_WARN("Frontend dist/index.html not found; SPA URLs may 404. Build vms-frontend and run backend from repo or copy dist next to the executable.");
    return cached;
}

crow::response serve_frontend_file_or_spa(const std::string& rel) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(resolve_frontend_dist_dir()), ec);
    if (ec || !fs::is_directory(root)) {
        return crow::response(503, "Frontend bundle not found (build vms-frontend and ensure dist/ is reachable).");
    }

    if (!rel.empty() && rel.find("..") != std::string::npos) {
        return crow::response(400, "Invalid path");
    }

    if (rel.empty()) {
        crow::response res;
        res.set_static_file_info((root / "index.html").string());
        return res;
    }

    fs::path target = fs::weakly_canonical(root / rel, ec);
    if (ec) {
        crow::response res;
        res.set_static_file_info((root / "index.html").string());
        return res;
    }

    auto rel_to_root = fs::relative(target, root, ec);
    if (ec || rel_to_root.string().find("..") != std::string::npos) {
        return crow::response(403, "Forbidden");
    }

    if (fs::is_regular_file(target)) {
        crow::response res;
        res.set_static_file_info(target.string());
        return res;
    }

    crow::response res;
    res.set_static_file_info((root / "index.html").string());
    return res;
}

} // namespace


HttpServer::HttpServer(const std::string& host, int port, int threads)
    : host_(host), port_(port), threads_(threads) {
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::run() {
    LOG_INFO("Starting HTTP Server on {}:{}", host_, port_);
    
    // Register all routes
    registerRoutes();
    
    // Configure App
    // Middleware is now enabled globally via VmsApp type definition in vms_app.h
    // Routes defined in registerRoutes() will be protected by AuthMiddleware automatically
    // unless excluded in AuthMiddleware::before_handle().

    // Start background services before entering Crow's blocking event loop.
    vms::core::AnalyticsCleanupService::getInstance().start();

    app_.bindaddr(host_)
        .port(port_)
        .concurrency(threads_)
        .loglevel(crow::LogLevel::Warning)
        .run();
}

void HttpServer::stop() {
    LOG_INFO("Stopping HTTP Server...");
    vms::core::AnalyticsCleanupService::getInstance().stop();
    app_.stop();
}

void HttpServer::registerRoutes() {
    LOG_INFO("Registering API routes...");
    
    // Register Controllers
    api::CameraController::registerRoutes(app_);
    api::ROIController::registerRoutes(app_);
    api::EventController::registerRoutes(app_);
    api::RecordingController::registerRoutes(app_);
    api::TrackingController::registerRoutes(app_);
    api::FaceController::registerRoutes(app_);
    api::ANPRController::registerRoutes(app_);
    api::UserController::registerRoutes(app_);
    api::SystemController::registerRoutes(app_); // Register System Routes
    api::SnapshotController::registerRoutes(app_);
    api::TrafficController::registerRoutes(app_);
    api::CameraDiscoveryController::registerRoutes(app_);
    api::ScannerController::registerRoutes(app_);
    api::EventEngineController::registerRoutes(app_);
    api::PTZController::registerRoutes(app_);
    api::HLSController::registerRoutes(app_);
    api::DeviceController::registerRoutes(app_);
    api::SiteController::registerRoutes(app_);
    api::AnalyticsController::registerRoutes(app_);
    api::VideoWallController::registerRoutes(app_);
    api::ReIDController::registerRoutes(app_);
    api::AlertController::registerRoutes(app_);
    
    // Reporting
    api::ReportingController::registerRoutes(app_);
    
    // Synopsis
    api::SynopsisController::registerRoutes(app_, auth_);
    
    // Export
    api::ExportController::registerRoutes(app_, auth_);

    // Old hardcoded auth routes replaced by UserController
    
    // Global WebSocket Route — system events and heartbeat
    // Uses Connection-ID pattern to prevent raw pointer dangling crashes.
    
    // WebSocket functionality migrated to QWebSocketServer in CameraStreamManager
    // Crow REST controllers are kept untouched.


    // Static Directories
    CROW_ROUTE(app_, "/snapshots/<string>")
    ([](const crow::request& /*req*/, std::string filename) {
        if (filename.find("..") != std::string::npos || 
            filename.find('/') != std::string::npos || 
            filename.find('\\') != std::string::npos) {
            return crow::response(400, "Invalid filename");
        }
        crow::response res;
        res.set_static_file_info("snapshots/" + filename);
        return res;
    });

    CROW_ROUTE(app_, "/recordings/<string>")
    ([](const crow::request& /*req*/, std::string filename) {
        if (filename.find("..") != std::string::npos || 
            filename.find('/') != std::string::npos || 
            filename.find('\\') != std::string::npos) {
            return crow::response(400, "Invalid filename");
        }
        crow::response res;
        res.set_static_file_info("recordings/" + filename);
        return res;
    });

    CROW_ROUTE(app_, "/api/storage/faces/<string>")
    ([](const crow::request& /*req*/, std::string filename) {
        if (filename.find("..") != std::string::npos || 
            filename.find('/') != std::string::npos || 
            filename.find('\\') != std::string::npos) {
            return crow::response(400, "Invalid filename");
        }
        crow::response res;
        res.set_static_file_info("storage/faces/" + filename);
        return res;
    });

    // -------------------------------------------------------------------------
    // Single-page app (vms-frontend/dist): must be registered after /api and
    // /snapshots etc. so those paths keep precedence.
    // -------------------------------------------------------------------------
    CROW_ROUTE(app_, "/")
    ([](const crow::request& req) {
        if (req.method != crow::HTTPMethod::Get) {
            return crow::response(405);
        }
        return serve_frontend_file_or_spa("");
    });

    CROW_ROUTE(app_, "/<path>")
    ([](const crow::request& req, std::string path) {
        if (req.method != crow::HTTPMethod::Get) {
            return crow::response(405);
        }
        return serve_frontend_file_or_spa(path);
    });

    LOG_INFO("All routes registered successfully");
}

} // namespace server
} // namespace vms
