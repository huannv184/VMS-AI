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
#include <QSqlError>
#include <chrono>
#include <sstream>

#include "utils/rate_limiter.h"
#include "utils/storage_manager.h"
#include "utils/metrics_auth.h"
#include "utils/config.h"
#include "core/readiness_state.h"
#include "database/db_state.h"
#include "events/alert_delivery.h"
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
    // Pre-fix this queried `WHERE status='online'` but the cameras schema
    // has no `status` column (only `is_active` — see db_manager.cpp
    // initializeTables). The query failed, the empty `catch(...){}` swallowed
    // the exception, and vms_cameras_active was always 0 → dashboard
    // appeared "no cameras running" forever. Use is_active (the operator's
    // intent flag) and surface query failure instead of swallowing.
    //
    // Note: is_active is the "configured to be on" flag, not "currently
    // streaming". A future enhancement could query PipelineStateStore for
    // true runtime state, but that requires touching the metrics endpoint
    // hot path with a different lock — defer.
    int total_cameras = 0, active_cameras = 0;
    if (db_up) {
        try {
            QSqlDatabase conn = db.getThreadConnection();
            if (conn.isOpen()) {
                QSqlQuery q(conn);
                if (q.exec("SELECT COUNT(*) FROM cameras") && q.next()) {
                    total_cameras = q.value(0).toInt();
                } else {
                    LOG_WARN("[metrics] cameras total query failed: {}", q.lastError().text().toStdString());
                }
                if (q.exec("SELECT COUNT(*) FROM cameras WHERE is_active=1") && q.next()) {
                    active_cameras = q.value(0).toInt();
                } else {
                    LOG_WARN("[metrics] cameras active query failed: {}", q.lastError().text().toStdString());
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("[metrics] cameras count exception: {}", e.what());
        }
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

    // ── storage (H7) ─────────────────────────────────────────────────────────
    auto& storage = vms::utils::StorageManager::getInstance();
    const bool storage_up = storage.isAvailable();
    const bool storage_required = storage.isRequired();
    out << "# HELP vms_storage_up Storage backend reachable (1=up, 0=down). Local driver is always 1.\n";
    out << "# TYPE vms_storage_up gauge\n";
    out << "vms_storage_up " << (storage_up ? 1 : 0) << "\n";
    out << "# HELP vms_storage_required Operator policy: 1=fail readiness on outage, 0=degraded-but-serving.\n";
    out << "# TYPE vms_storage_required gauge\n";
    out << "vms_storage_required " << (storage_required ? 1 : 0) << "\n";

    // ── readiness (H6 health split — derived from same snapshot the /api/health/ready route uses) ──
    vms::core::ReadinessSnapshot ready_snap{};
    ready_snap.db_ready = db_up;
    ready_snap.storage_ready = storage_up;
    ready_snap.storage_required = storage_required;
    const bool ready = vms::core::computeReady(ready_snap);
    out << "# HELP vms_ready Backend is ready to serve traffic (1=ready, 0=not_ready). Mirrors /api/health/ready.\n";
    out << "# TYPE vms_ready gauge\n";
    out << "vms_ready " << (ready ? 1 : 0) << "\n";

    // ── alert delivery pools (per-channel + aggregate) ────────────────────────
    // Source-of-truth: events::deliveryStats() returns the same JSON shape
    // operators see on /api/rules/stats.delivery. We re-emit it as Prometheus
    // gauges/counters so alerts can fire on queue backpressure or drop rate
    // without hitting the higher-RBAC /api/rules/stats endpoint.
    try {
        const auto delivery_json = vms::events::deliveryStats();
        for (const char* channel : {"webhook", "sms", "telegram", "alarm"}) {
            const auto& ch = delivery_json[channel];
            const std::uint64_t cur_q = ch.value("current_queue_depth", static_cast<std::uint64_t>(0));
            const std::uint64_t max_q = ch.value("max_queue_size", static_cast<std::uint64_t>(0));
            const std::uint64_t peak_q = ch.value("peak_queue_depth", static_cast<std::uint64_t>(0));
            const std::uint64_t submitted = ch.value("submitted_total", static_cast<std::uint64_t>(0));
            const std::uint64_t dropped = ch.value("dropped_total", static_cast<std::uint64_t>(0));
            out << "vms_alert_delivery_queue_depth{channel=\"" << channel << "\"} " << cur_q << "\n";
            out << "vms_alert_delivery_queue_capacity{channel=\"" << channel << "\"} " << max_q << "\n";
            out << "vms_alert_delivery_peak_queue_depth{channel=\"" << channel << "\"} " << peak_q << "\n";
            out << "vms_alert_delivery_submitted_total{channel=\"" << channel << "\"} " << submitted << "\n";
            out << "vms_alert_delivery_dropped_total{channel=\"" << channel << "\"} " << dropped << "\n";
        }
        out << "# HELP vms_alert_delivery_queue_depth Current queued jobs per channel.\n";
        out << "# TYPE vms_alert_delivery_queue_depth gauge\n";
        out << "# HELP vms_alert_delivery_queue_capacity Per-channel queue size (drop threshold).\n";
        out << "# TYPE vms_alert_delivery_queue_capacity gauge\n";
        out << "# HELP vms_alert_delivery_peak_queue_depth Peak observed queue depth (high water).\n";
        out << "# TYPE vms_alert_delivery_peak_queue_depth gauge\n";
        out << "# HELP vms_alert_delivery_submitted_total Jobs accepted into the per-channel pool.\n";
        out << "# TYPE vms_alert_delivery_submitted_total counter\n";
        out << "# HELP vms_alert_delivery_dropped_total Jobs dropped (queue full + not-accepting).\n";
        out << "# TYPE vms_alert_delivery_dropped_total counter\n";
    } catch (...) {
        // deliveryStats() is best-effort here — scrape must not 500 if a
        // runner is transitioning state mid-scrape.
    }

    // ── DB batch writer ──────────────────────────────────────────────────────
    const auto bw = db.batchWriterStats();
    out << "# HELP vms_db_batch_queue_depth Current event-batch-writer queue depth.\n";
    out << "# TYPE vms_db_batch_queue_depth gauge\n";
    out << "vms_db_batch_queue_depth " << bw.current_queue_depth << "\n";
    out << "# HELP vms_db_batch_peak_queue_depth Peak observed queue depth (high water).\n";
    out << "# TYPE vms_db_batch_peak_queue_depth gauge\n";
    out << "vms_db_batch_peak_queue_depth " << bw.peak_queue_depth << "\n";
    out << "# HELP vms_db_batch_enqueued_total Events accepted into the batch queue.\n";
    out << "# TYPE vms_db_batch_enqueued_total counter\n";
    out << "vms_db_batch_enqueued_total " << bw.enqueued_total << "\n";
    out << "# HELP vms_db_batch_dropped_total Events dropped (queue full or not-accepting).\n";
    out << "# TYPE vms_db_batch_dropped_total counter\n";
    out << "vms_db_batch_dropped_total " << bw.dropped_total << "\n";
    out << "# HELP vms_db_batch_flushed_total Events successfully persisted.\n";
    out << "# TYPE vms_db_batch_flushed_total counter\n";
    out << "vms_db_batch_flushed_total " << bw.flushed_total << "\n";
    out << "# HELP vms_db_batch_flush_failures_total Batch commit failures (transaction aborts).\n";
    out << "# TYPE vms_db_batch_flush_failures_total counter\n";
    out << "vms_db_batch_flush_failures_total " << bw.flush_failures_total << "\n";
    out << "# HELP vms_db_batch_row_failures_total Per-row insert failures (poisoned events).\n";
    out << "# TYPE vms_db_batch_row_failures_total counter\n";
    out << "vms_db_batch_row_failures_total " << bw.row_failures_total << "\n";

    // ── WebSocket connection caps + lifecycle ─────────────────────────────────
    try {
        const auto ws = vms::streaming::CameraStreamManager::getInstance().connectionStats();
        out << "# HELP vms_ws_connections_current Active WebSocket connections.\n";
        out << "# TYPE vms_ws_connections_current gauge\n";
        out << "vms_ws_connections_current " << ws.connections_current << "\n";
        out << "# HELP vms_ws_peak_connections Peak concurrent WS connections since boot.\n";
        out << "# TYPE vms_ws_peak_connections gauge\n";
        out << "vms_ws_peak_connections " << ws.peak_connections << "\n";
        out << "# HELP vms_ws_connections_total Total WS connections accepted since boot.\n";
        out << "# TYPE vms_ws_connections_total counter\n";
        out << "vms_ws_connections_total " << ws.connections_total << "\n";
        out << "# HELP vms_ws_authed_total Connections that completed AUTH handshake.\n";
        out << "# TYPE vms_ws_authed_total counter\n";
        out << "vms_ws_authed_total " << ws.authed_total << "\n";
        out << "# HELP vms_ws_auth_failed_total Connections rejected for bad/missing token.\n";
        out << "# TYPE vms_ws_auth_failed_total counter\n";
        out << "vms_ws_auth_failed_total " << ws.auth_failed_total << "\n";
        out << "# HELP vms_ws_conn_cap_rejected_total Connections rejected by accept-time cap (max_connections_*).\n";
        out << "# TYPE vms_ws_conn_cap_rejected_total counter\n";
        out << "vms_ws_conn_cap_rejected_total " << ws.conn_cap_rejected_total << "\n";
        out << "# HELP vms_ws_disconnects_total Lifecycle: total disconnects since boot.\n";
        out << "# TYPE vms_ws_disconnects_total counter\n";
        out << "vms_ws_disconnects_total " << ws.disconnects_total << "\n";
    } catch (...) {
        // CameraStreamManager may be mid-shutdown; tolerate.
    }

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
    // 2026-05-14: AlertController retired in AlertManager consolidation.
    // /api/alerts/rules CRUD is gone; rule editing happens via /api/rules*.
    // Legacy alert_rules rows are auto-migrated by
    // RuleEngine::migrateLegacyAlertRules at boot.
    api::AttendanceController::registerRoutes(app_);
    api::CounterController::registerRoutes(app_);

    // Reporting
    api::ReportingController::registerRoutes(app_);
    
    // Synopsis
    api::SynopsisController::registerRoutes(app_, auth_);
    
    // Export
    api::ExportController::registerRoutes(app_, auth_);

    // Old hardcoded auth routes replaced by UserController

    // GET /api/v1/metrics — Prometheus scrape endpoint.
    //
    // H9: optional static-bearer-token gate. When SecurityConfig::metrics_token
    // is empty the endpoint stays unauthenticated at the route level —
    // operators rely on a network ACL (firewall / k8s NetworkPolicy / nginx
    // location block) for restriction. When the token is configured (yaml
    // security.metrics_token or VMS_METRICS_TOKEN env), requests must carry
    // `Authorization: Bearer <token>` matching it (constant-time compared in
    // isMetricsRequestAuthorized — guards against timing/length oracles).
    // AuthMiddleware allowlists this path so JWT is bypassed here; the bearer
    // gate IS the entire auth for this one route.
    //
    // Why a separate token vs JWT: Prometheus scrapes every 15-60 s and
    // expects a static bearer (`bearer_token_file`). A short-lived JWT
    // would need an external refresher (operationally painful).
    CROW_ROUTE(app_, "/api/v1/metrics")
    .methods(crow::HTTPMethod::Get)
    ([](const crow::request& req) {
        const auto& token = vms::Config::getInstance().getSecurityConfig().metrics_token;
        const std::string auth = req.get_header_value("Authorization");
        if (!vms::utils::isMetricsRequestAuthorized(token, auth)) {
            crow::response res(401);
            res.set_header("WWW-Authenticate", "Bearer realm=\"vms-metrics\"");
            return res;
        }
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
