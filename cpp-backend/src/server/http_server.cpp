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
#include "api/attendance_controller.h"
#include "api/counter_controller.h"
#include "middleware/auth_middleware.h"
#include "utils/logger.h"
#include "utils/api_utils.h"
#include "core/analytics_cleanup_service.h"
#include "streaming/camera_stream_manager_qt.h"
#include "database/db_manager.h"

#include <filesystem>
#include <QSqlQuery>
#include <chrono>
#include <sstream>

#include "utils/rate_limiter.h"
#include "ipc/zmq_event_bridge.h"

namespace vms {
namespace server {

namespace {

// Prometheus text format: one metric per line, "# HELP / # TYPE" optional but good practice.
static std::string buildPrometheusMetrics() {
    std::ostringstream out;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto& db = database::DbManager::getInstance();
    const bool db_up = db.isInitialized();

    // ── database ─────────────────────────────────────────────────────────────
    out << "# HELP vms_db_up Database connection status (1=up, 0=down)\n";
    out << "# TYPE vms_db_up gauge\n";
    out << "vms_db_up " << (db_up ? 1 : 0) << "\n";

    // ── cameras ───────────────────────────────────────────────────────────────
    int total_cameras = 0, active_cameras = 0;
    if (db_up) {
        try {
            QSqlDatabase conn = db.getThreadConnection();
            if (conn.isOpen()) {
                QSqlQuery q(conn);
                if (q.exec("SELECT COUNT(*) FROM cameras") && q.next())
                    total_cameras = q.value(0).toInt();
                if (q.exec("SELECT COUNT(*) FROM cameras WHERE status='online'") && q.next())
                    active_cameras = q.value(0).toInt();
            }
        } catch (...) {}
    }
    out << "# HELP vms_cameras_total Total registered cameras\n";
    out << "# TYPE vms_cameras_total gauge\n";
    out << "vms_cameras_total " << total_cameras << "\n";
    out << "# HELP vms_cameras_active Cameras currently online\n";
    out << "# TYPE vms_cameras_active gauge\n";
    out << "vms_cameras_active " << active_cameras << "\n";

    // ── events (last 24h) ────────────────────────────────────────────────────
    int events_24h = 0;
    if (db_up) {
        try {
            QSqlDatabase conn = db.getThreadConnection();
            if (conn.isOpen()) {
                QSqlQuery q(conn);
                long long since = (now_ms / 1000) - 86400;
                q.prepare("SELECT COUNT(*) FROM events WHERE timestamp >= ?");
                q.bindValue(0, static_cast<qlonglong>(since));
                if (q.exec() && q.next())
                    events_24h = q.value(0).toInt();
            }
        } catch (...) {}
    }
    out << "# HELP vms_events_24h Events generated in the last 24 hours\n";
    out << "# TYPE vms_events_24h gauge\n";
    out << "vms_events_24h " << events_24h << "\n";

    // ── process uptime ────────────────────────────────────────────────────────
    static const long long start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out << "# HELP vms_uptime_seconds Process uptime in seconds\n";
    out << "# TYPE vms_uptime_seconds counter\n";
    out << "vms_uptime_seconds " << (now_ms - start_ms) / 1000 << "\n";

    // ── rate limiter ──────────────────────────────────────────────────────────
    out << "# HELP vms_rate_limiter_tracked_entries Current number of IPs/users tracked by RateLimiter\n";
    out << "# TYPE vms_rate_limiter_tracked_entries gauge\n";
    out << "vms_rate_limiter_tracked_entries " << vms::utils::RateLimiter::getInstance().getTrackedEntriesCount() << "\n";

    // ── zmq bridge ────────────────────────────────────────────────────────────
    auto& zmq = vms::ipc::ZmqEventBridge::getInstance();
    out << "# HELP vms_zmq_messages_received Total ZMQ messages received from AI Workers\n";
    out << "# TYPE vms_zmq_messages_received counter\n";
    out << "vms_zmq_messages_received " << zmq.getMessagesReceived() << "\n";
    out << "# HELP vms_zmq_messages_dropped Total ZMQ messages dropped due to invalid JSON or HMAC\n";
    out << "# TYPE vms_zmq_messages_dropped counter\n";
    out << "vms_zmq_messages_dropped " << zmq.getMessagesDropped() << "\n";

    return out.str();
}

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

// Serve a file from the on-disk `storage/` directory used by face_controller
// (writes /api/storage/faces/...) and reid_engine (writes
// storage/reid_thumbnails/...). Without this route the catch-all below would
// hand back the SPA's index.html for every image URL — face avatars and
// ReID thumbnails would render as a chunk of HTML.
//
// The route is GET-only; auth_middleware whitelists /api/storage/faces/ as
// public media, and other subdirs require a valid session because the
// middleware runs before this handler.
crow::response serve_storage_file(const std::string& rel) {
    namespace fs = std::filesystem;

    // Reject ".." anywhere — both raw and percent-decoded — and absolute paths.
    if (rel.empty()) return crow::response(404);
    if (rel.find("..") != std::string::npos) return crow::response(400, "Invalid path");
    if (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) return crow::response(400, "Invalid path");

    std::error_code ec;
    const fs::path storage_root = fs::weakly_canonical(fs::path("storage"), ec);
    if (ec) return crow::response(404);

    const fs::path target = fs::weakly_canonical(storage_root / rel, ec);
    if (ec) return crow::response(404);

    // Defence in depth: ensure the resolved path is still under storage_root.
    auto rel_to_root = fs::relative(target, storage_root, ec);
    if (ec || rel_to_root.empty() || rel_to_root.string().rfind("..", 0) == 0) {
        return crow::response(403, "Forbidden");
    }

    if (!fs::is_regular_file(target)) return crow::response(404);

    crow::response res;
    res.set_static_file_info(target.string());
    // Static media — let browsers cache aggressively. Filenames embed a
    // timestamp so cache-busting happens naturally on update.
    res.set_header("Cache-Control", "public, max-age=86400");
    return res;
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

    if (!configured_) {
        registerRoutes();
        vms::core::AnalyticsCleanupService::getInstance().start();
        app_.bindaddr(host_)
            .port(port_)
            .concurrency(threads_)
            .loglevel(crow::LogLevel::Warning);
        configured_ = true;
    }

    app_.run();
}

std::future<void> HttpServer::runAsync() {
    LOG_INFO("Starting HTTP Server on {}:{} (async)", host_, port_);

    if (!configured_) {
        registerRoutes();
        vms::core::AnalyticsCleanupService::getInstance().start();
        app_.bindaddr(host_)
            .port(port_)
            .concurrency(threads_)
            .loglevel(crow::LogLevel::Warning);
        configured_ = true;
    }

    // Crow's run_async() returns std::future<void> from std::async; the future's
    // shared state captures any exception thrown by run() (bind failure, duplicate
    // route — see BUG-HTTP-01). Caller must keep this future alive AND poll it.
    return app_.run_async();
}

bool HttpServer::waitForStart(std::chrono::milliseconds timeout) {
    return app_.wait_for_server_start(timeout) == std::cv_status::no_timeout;
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
    api::EventEngineController::registerRoutes(app_);
    api::PTZController::registerRoutes(app_);
    api::HLSController::registerRoutes(app_);
    api::DeviceController::registerRoutes(app_);
    api::SiteController::registerRoutes(app_);
    api::AnalyticsController::registerRoutes(app_);
    api::VideoWallController::registerRoutes(app_);
    api::ReIDController::registerRoutes(app_);
    api::AlertController::registerRoutes(app_);
    api::AttendanceController::registerRoutes(app_);
    api::CounterController::registerRoutes(app_);

    // Reporting
    api::ReportingController::registerRoutes(app_);
    
    // Synopsis
    api::SynopsisController::registerRoutes(app_, auth_);
    
    // Export
    api::ExportController::registerRoutes(app_, auth_);

    // Old hardcoded auth routes replaced by UserController

    // GET /api/v1/metrics — Prometheus scrape endpoint (no auth — restrict via network ACL)
    CROW_ROUTE(app_, "/api/v1/metrics")
    .methods(crow::HTTPMethod::Get)
    ([]() {
        crow::response res(200, buildPrometheusMetrics());
        res.set_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        return res;
    });

    // Global WebSocket Route — system events and heartbeat
    // WebSocket functionality migrated to QWebSocketServer in CameraStreamManager
    // Crow REST controllers are kept untouched.

    // -------------------------------------------------------------------------
    // Storage media (face avatars, reid thumbnails, ...)
    // MUST be registered before the SPA catch-all so /api/storage/... and
    // /storage/... resolve to actual files instead of falling through to
    // index.html.
    // -------------------------------------------------------------------------
    CROW_ROUTE(app_, "/api/storage/<path>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req, std::string path) {
        if (req.method == crow::HTTPMethod::Options) {
            crow::response res(204);
            res.set_header("Access-Control-Allow-Origin",
                vms::api::ApiUtils::resolveCorsOrigin(req));
            return res;
        }
        return serve_storage_file(path);
    });

    CROW_ROUTE(app_, "/storage/<path>")
    .methods(crow::HTTPMethod::Get)
    ([](const crow::request& req, std::string path) {
        (void)req;
        return serve_storage_file(path);
    });

    // -------------------------------------------------------------------------
    // Single-page app (vms-frontend/dist)
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
