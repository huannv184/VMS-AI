// Include Server/Crow first to handle WinSock/Windows.h ordering
#include "server/http_server.h"

#include <iostream>
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>

// Windows includes after network includes
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod / timeEndPeriod
#include <crtdbg.h>   // For _CrtSetReportMode
#endif

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include "utils/logger.h"
#include "utils/config.h"
#include "utils/qt_logger.h"
#include "utils/storage_manager.h"
#include "api/export_controller.h"
#include "api/synopsis_controller.h"
#include "database/db_manager.h"
#include "database/db_state.h"
#include "core/camera_manager.h"
#include "core/camera_pipeline_manager.h"
#include "core/process_manager.h"
#include "core/runtime_state.h"
#include "core/roi_manager.h"
#include "core/event_manager.h"
#include "core/reid_engine.h"
#include "core/attendance_tracker.h"
#include "core/counter_bucket_aggregator.h"
#include "events/alert_delivery.h"
#include "events/rule_engine.h"
#include "events/zone_manager.h"
#include "streaming/camera_stream_manager_qt.h"
#include "ipc/zmq_event_bridge.h"
#include <curl/curl.h>

namespace {

std::atomic<bool> g_shutdown_sequence_started{false};
QCoreApplication* g_qt_app = nullptr;
vms::server::HttpServer* g_http_server = nullptr;
#ifdef _WIN32
HANDLE g_shutdown_complete_event = nullptr;
#endif

void request_shutdown_async(const char* reason) {
    bool expected = false;
    const bool first_request = g_shutdown_sequence_started.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel);

    vms::core::shutdown_requested.store(true, std::memory_order_release);
    vms::core::shutting_down.store(true, std::memory_order_release);

    if (!first_request) {
        return;
    }

    std::cout << "\n[SHUTDOWN] " << reason << '\n' << std::flush;

    if (!g_qt_app) {
        return;
    }

    QMetaObject::invokeMethod(g_qt_app, []() {
        QCoreApplication::quit();
    }, Qt::QueuedConnection);
}

} // namespace

void signal_handler(int signal) {
    const char* signal_name = "UNKNOWN";
    switch (signal) {
        case SIGINT:  signal_name = "SIGINT (Ctrl+C)"; break;
        case SIGTERM: signal_name = "SIGTERM"; break;
        case SIGABRT: signal_name = "SIGABRT"; break;
    }

    request_shutdown_async(signal_name);
}

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
            request_shutdown_async("CTRL_C_EVENT");
            return TRUE;
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            request_shutdown_async("Windows console termination");
            if (g_shutdown_complete_event) {
                WaitForSingleObject(g_shutdown_complete_event, 15000);
            }
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* ExceptionInfo) {
    DWORD code = ExceptionInfo->ExceptionRecord->ExceptionCode;
    LOG_ERROR("========================================");
    LOG_ERROR("UNHANDLED EXCEPTION!");
    LOG_ERROR("Exception Code: 0x{:X}", code);
    LOG_ERROR("Exception Address: 0x{:X}", (uintptr_t)ExceptionInfo->ExceptionRecord->ExceptionAddress);
    LOG_ERROR("========================================");
    
    // 0x80000003 = STATUS_BREAKPOINT — triggered by debug assertions,
    // CRT debug heap checks, or __debugbreak(). These are non-fatal in
    // background threads and should NOT kill the entire process.
    if (code == 0x80000003 || code == 0x80000004) {
        LOG_WARN("Breakpoint/single-step exception caught — continuing execution");
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char* argv[]) {
    std::cout << "Starting Monolithic VMS Backend..." << std::endl;

    SetUnhandledExceptionFilter(CustomUnhandledExceptionFilter);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGABRT, signal_handler);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

    // FIX: Disable debug CRT pop-up dialogs that trigger 0x80000003 breakpoints
    // Redirect assertion failures to stderr instead of triggering a debugger break
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    
    // Reduce Windows timer resolution from 15.6ms → 1ms.
    // Without this: QThread::msleep(1) actually sleeps 15ms, causing ±15ms frame pacing jitter.
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    // Create QCoreApplication BEFORE any QObject/QTimer/QThread usage
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("VMS AI Backend");
    QCoreApplication::setApplicationVersion("1.1");
    g_qt_app = &app;
#ifdef _WIN32
    g_shutdown_complete_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif

    try {
        // Initialize libcurl globally for multi-threaded HTTP/ONVIF API calls
        curl_global_init(CURL_GLOBAL_ALL);

        char cwd[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, cwd);
        std::cout << "Data Directory: " << cwd << std::endl;

        std::cout << "============================================" << std::endl;
        std::cout << "  VMS AI Backend v1.1" << std::endl;

        // Load configuration — try multiple fallbacks
        std::string config_path;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
                config_path = argv[++i];
            } else if (arg.find(".yaml") != std::string::npos || arg.find(".yml") != std::string::npos) {
                config_path = arg;
            }
        }

        if (config_path.empty()) {
            std::vector<std::string> candidates = {
                "config/backend.yaml",
                "../../config/backend.yaml",    // Running from build/Release/
                "../config/backend.yaml"        // Running from build/
            };
            for (const auto& c : candidates) {
                if (std::filesystem::exists(c)) {
                    config_path = c;
                    break;
                }
            }
        }

        if (config_path.empty()) {
            std::cerr << "FATAL: No configuration file specified and none found in default locations." << std::endl;
            std::cerr << "       Pass --config <path> or place backend.yaml in one of: config/, ../../config/, ../config/" << std::endl;
            return EXIT_FAILURE;
        }

        std::string absolute_config_path = std::filesystem::absolute(config_path).string();
        std::cout << "Loading configuration from: " << absolute_config_path << std::endl;

        // 2026-05-15: distinguish "couldn't read/parse the YAML" from "YAML
        // loaded fine but a validation check rejected it" (JWT default secret,
        // missing required keys, etc). Pre-fix the cerr below claimed
        // "Failed to load configuration from: <path>" for BOTH cases —
        // operators chased file-path issues when the real reason was the JWT
        // sentinel inside Config::loadFromFile. Now we do a separate
        // file-existence + readability probe so we can give a specific message
        // when the file itself is the problem; otherwise the cerr says
        // "validation failed" and points at the preceding LOG_ERROR.
        std::error_code probe_ec;
        if (!std::filesystem::exists(config_path, probe_ec) || probe_ec) {
            std::cerr << "FATAL: Configuration file does not exist: " << absolute_config_path
                      << (probe_ec ? std::string(" (") + probe_ec.message() + ")" : std::string())
                      << std::endl;
            return EXIT_FAILURE;
        }
        if (std::filesystem::file_size(config_path, probe_ec) == 0 || probe_ec) {
            std::cerr << "FATAL: Configuration file is empty or unreadable: " << absolute_config_path
                      << (probe_ec ? std::string(" (") + probe_ec.message() + ")" : std::string())
                      << std::endl;
            return EXIT_FAILURE;
        }

        if (!vms::Config::getInstance().loadFromFile(config_path)) {
            // Config::loadFromFile already emitted a specific LOG_ERROR for
            // the reason (YAML parse error, JWT sentinel, short secret, etc.).
            // The spdlog console sink is async (overrun_oldest); plain
            // logger->flush() just enqueues a flush request and returns —
            // not synchronous. Call spdlog::shutdown() to drain + join the
            // worker pool so the [error] log line appears in causal order
            // before this cerr. We're about to return EXIT_FAILURE anyway,
            // so loss of the logger is fine.
            spdlog::shutdown();
            std::cerr << "FATAL: Configuration load or validation failed for "
                      << absolute_config_path << "." << std::endl;
            std::cerr << "       See the preceding [error] log line(s) above for the specific reason "
                      << "(common causes: YAML parse error, default JWT secret, secret < 32 chars)."
                      << std::endl;
            return EXIT_FAILURE;
        }
        LOG_INFO("Configuration loaded successfully.");
        vms::Config& config = vms::Config::getInstance();
        
        const auto& logConfig = config.getLoggingConfig();
        vms::Logger::init(
            logConfig.file_path,
            logConfig.level,
            static_cast<size_t>(logConfig.max_file_size_mb),
            static_cast<size_t>(logConfig.max_files),
            logConfig.console_output,
            logConfig.file_output
        );
        vms::utils::installQtLogBridge();

        LOG_INFO("============================================");
        LOG_INFO("VMS AI Backend v1.1 Starting...");
        LOG_INFO("============================================");

        config.print();
        
        const auto& serverConfig = config.getServerConfig();
        std::string host = serverConfig.host;
        int port = serverConfig.port;
        const unsigned hw_threads = std::max(1u, std::thread::hardware_concurrency());
        const int threads = serverConfig.threads > 0
            ? serverConfig.threads
            : static_cast<int>(hw_threads);

        vms::database::DbManager& db_manager = vms::database::DbManager::getInstance();

        // ── Phase 1: Database Initialization ──
        LOG_INFO("Initializing database...");
        try {
            if (!db_manager.init(config.getDatabaseConfig())) {
                LOG_ERROR("Database initialization failed. App may not function correctly.");
            } else {
                LOG_INFO("Database initialized successfully");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Database init exception: {}", e.what());
        } catch (...) {
            LOG_ERROR("Database init unknown exception");
        }

        LOG_INFO("Initializing HTTP server...");
        vms::server::HttpServer http_server(host, port, threads);
        g_http_server = &http_server;

        LOG_INFO("Starting HTTP server on {}:{} with {} threads (hardware_concurrency override from config={})",
                 host, port, threads, serverConfig.threads);

        // BUG-HTTP-01 followup: previously we spawned std::thread([&]{ http_server.run(); })
        // and slept 100ms. If Crow's run() threw (duplicate CROW_ROUTE, bind failure,
        // etc.) the thread died silently, the LOG_ERROR landed buried in logs, and the
        // main thread proceeded to log "Backend started successfully!" with no listener
        // on port 8000. Frontend then got ECONNREFUSED on every /api/* call.
        //
        // Fix: drive Crow with run_async() so we hold a future, then wait for the
        // server-started condvar AND poll the future. If the future is ready before
        // wait_for_server_start unblocks, run() exited (= died); calling .get() on
        // that future re-throws the original exception so we fail-fast at startup
        // instead of pretending we're up.
        auto server_future = http_server.runAsync();

        const bool server_listening = http_server.waitForStart(std::chrono::seconds(5));
        if (!server_listening) {
            // Either still starting (unlikely past 5s) or died. Distinguish by
            // checking the future — if it's ready, run() returned/threw.
            if (server_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    server_future.get();  // re-throws any exception from run()
                } catch (const std::exception& e) {
                    LOG_ERROR("HTTP server failed to start: {}", e.what());
                    throw;
                }
                // run() returned without throwing but server isn't listening —
                // still a fatal startup failure (Crow shouldn't return cleanly
                // before stop() was called).
                throw std::runtime_error("HTTP server exited before bind completed");
            }
            // Future not ready and server not signalling start within 5s. Treat
            // as fatal — no point bringing up the rest of the stack if the API
            // surface is unreachable.
            throw std::runtime_error("HTTP server did not start within 5 seconds");
        }

        LOG_INFO("Initializing storage manager...");
        try {
            if (!vms::utils::StorageManager::getInstance().init(config.getStorageConfig())) {
                LOG_WARN("Failed to initialize StorageManager (MinIO not available?). Backend will continue without storage.");
            }
        } catch (const std::exception& e) {
            LOG_WARN("StorageManager init exception: {}. Backend will continue without storage.", e.what());
        }

        // CRITICAL FIX: Preload CameraPipelineManager BEFORE CameraManager starts threads!
        // This prevents static initialization deadlocks on getInstance() when auto-starting cameras
        // and parsing YOLO engines concurrently.
        vms::core::CameraPipelineManager& pipeline_manager = vms::core::CameraPipelineManager::getInstance();

        LOG_INFO("Initializing managers...");

        vms::core::CameraManager& camera_manager = vms::core::CameraManager::getInstance();
        try {
            if (!camera_manager.init()) {
                LOG_WARN("CameraManager initialization incomplete (DB may be unavailable).");
            } else {
                // Since DB is synchronous, we can just trigger onDatabaseReady now if needed, 
                // but CameraManager::init() already reads from the DB.
            }
        } catch (const std::exception& e) {
            LOG_WARN("CameraManager init exception: {}. Continuing without camera data.", e.what());
        }

        vms::core::ROIManager& roi_manager = vms::core::ROIManager::getInstance();
        try {
            if (!roi_manager.init()) {
                LOG_WARN("ROIManager initialization incomplete. Will recover when DB is ready.");
            }
        } catch (const std::exception& e) {
            LOG_WARN("ROIManager init exception: {}. Continuing.", e.what());
        }

        vms::core::EventManager& event_manager = vms::core::EventManager::getInstance();
        try {
            if (!event_manager.init()) {
                LOG_WARN("EventManager initialization incomplete. Will recover when DB is ready.");
            }
        } catch (const std::exception& e) {
            LOG_WARN("EventManager init exception: {}. Continuing.", e.what());
        }

        // Load persisted zones from DB into ZoneManager (must be after DB init)
        try {
            vms::events::ZoneManager::getInstance().loadFromDb();
        } catch (const std::exception& e) {
            LOG_WARN("ZoneManager loadFromDb exception: {}", e.what());
        }

        // Load persisted rules from DB into RuleEngine. Without this, rules
        // created via the REST API survive a single process lifetime only —
        // restarting the backend wipes operator-configured detection rules.
        try {
            vms::events::RuleEngine::getInstance().loadFromDatabase();
        } catch (const std::exception& e) {
            LOG_WARN("RuleEngine loadFromDatabase exception: {}", e.what());
        }

        // One-shot migration from the legacy alert_rules table into the
        // unified rules table. Idempotent via the `alert_rules_migrated`
        // setting — only does real work on the first boot after the
        // 2026-05-14 AlertManager retirement.
        try {
            vms::events::RuleEngine::getInstance().migrateLegacyAlertRules();
        } catch (const std::exception& e) {
            LOG_WARN("RuleEngine migrateLegacyAlertRules exception: {}", e.what());
        }

        // Initialise ReIDEngine. Pre-fix this getInstance() was only called
        // from the REST controller, which doesn't call init(). The engine's
        // initialized_ flag stayed false forever → processDetection short-
        // circuited on `if (!initialized_) return -1` → /api/reid/gallery
        // and /api/reid/search always returned empty. Closes the boot side
        // of BUG-REID-DEAD-PIPELINE; the producer side is wired in
        // AiEventProcessor::processReID. Falls back to color-histogram
        // embedding if the ONNX model is missing (logged at WARN, not fatal).
        try {
            vms::core::ReIDEngine::getInstance().init();
            // Rehydrate cross-camera gallery from DB + start the 60s flush
            // thread. Without this the gallery cold-starts every restart;
            // operator-built identities and last-30min cross-camera mappings
            // are lost. Idempotent: skips silently if DB not yet ready.
            vms::core::ReIDEngine::getInstance().loadFromDatabase();
        } catch (const std::exception& e) {
            LOG_WARN("ReIDEngine init exception: {}", e.what());
        }

        LOG_INFO("All managers initialized successfully");

        // ── Start AttendanceTracker (depends on DB ready) ────────────────────
        // Reads employees + camera_roles into in-memory cache; spawns BulkWriter
        // thread that flushes attendance_events. Must start AFTER DB init and
        // BEFORE ZmqEventBridge so face events have a sink.
        try {
            vms::core::AttendanceTracker::getInstance().start();
        } catch (const std::exception& e) {
            LOG_WARN("AttendanceTracker start exception: {}", e.what());
        }

        // ── Start CounterBucketAggregator (rolls LINE_CROSSING_* → counter_buckets_1m) ──
        // Pure consumer of events table — does not need a sink. Worker thread
        // ticks every 60s and re-aggregates the last 5 min (UPSERT, idempotent).
        try {
            vms::core::CounterBucketAggregator::getInstance().start();
        } catch (const std::exception& e) {
            LOG_WARN("CounterBucketAggregator start exception: {}", e.what());
        }

        // ── Phase 1: Start ZeroMQ AI Event Bridge ────────────────────────────
        // Subscribes to AI Worker PUB on tcp://127.0.0.1:5555 (configurable via settings)
        // If AI Worker is offline the bridge retries silently without crashing the backend.
        if (config.getAIServerConfig().enabled) {
            LOG_INFO("Starting ZeroMQ Event Bridge...");
            vms::ipc::ZmqEventBridge::getInstance().start();
        } else {
            LOG_INFO("ZeroMQ Event Bridge disabled by configuration");
        }

        // ── Phase C: Start QWebSocketServer ──────────────────────────────────
        vms::streaming::CameraStreamManager::getInstance().start();

        // Wire ZmqEventBridge -> CameraStreamManager (AI Events to WebSocket)
        QObject::connect(&vms::ipc::ZmqEventBridge::getInstance(), &vms::ipc::ZmqEventBridge::eventReceived,
                         &vms::streaming::CameraStreamManager::getInstance(), &vms::streaming::CameraStreamManager::handleAiEvent);

        // Final liveness check — covers the (rare) window where the HTTP server
        // signalled "started" but a subsequent throw inside a deferred init
        // path on its thread (e.g. ssl handshake on first accept) tore it down
        // while the rest of the bring-up was running. If the future is ready,
        // the server already exited.
        if (server_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                server_future.get();
            } catch (const std::exception& e) {
                LOG_ERROR("HTTP server exited during startup: {}", e.what());
                throw;
            }
            throw std::runtime_error("HTTP server exited during startup before announce");
        }

        LOG_INFO("============================================");
        LOG_INFO("Backend started successfully!");
        LOG_INFO("API endpoints:");
        LOG_INFO("  - http://{}:{}/", host, port);
        LOG_INFO("  - http://{}:{}/api/health", host, port);
        LOG_INFO("============================================");
        LOG_INFO("Press Ctrl+C to stop");

        // ── Run Qt event loop (replaces while(!g_shutdown_requested) spin) ──
        int result = app.exec();

        // ── Shutdown sequence (reached after QCoreApplication::quit()) ──────
        LOG_INFO("========================================");
        LOG_INFO("Shutting down gracefully...");
        LOG_INFO("========================================");
        vms::core::shutdown_requested.store(true, std::memory_order_release);
        vms::core::shutting_down.store(true, std::memory_order_release);

        // ── Phase 1: Stop Producers ─────────────────────────────────────
        if (config.getAIServerConfig().enabled) {
            LOG_INFO("Stopping ZeroMQ Event Bridge...");
            vms::ipc::ZmqEventBridge::getInstance().stop();
        }

        // Stop AttendanceTracker AFTER ZmqEventBridge so no new face events
        // race with BulkWriter shutdown drain (drain runs on this thread and
        // requires DB still open — DbManager::close() happens later).
        try {
            vms::core::AttendanceTracker::getInstance().stop();
        } catch (const std::exception& e) {
            LOG_WARN("AttendanceTracker stop exception: {}", e.what());
        }

        // Stop CounterBucketAggregator BEFORE DbManager.close() — its worker
        // thread may be mid-sweep against the events table.
        try {
            vms::core::CounterBucketAggregator::getInstance().stop();
        } catch (const std::exception& e) {
            LOG_WARN("CounterBucketAggregator stop exception: {}", e.what());
        }

        LOG_INFO("Stopping camera pipelines...");
        pipeline_manager.stopAllPipelines();

        LOG_INFO("Stopping camera manager...");
        camera_manager.stopAll();

        // ── Phase 2: Stop Consumers ─────────────────────────────────────
        LOG_INFO("Stopping stream delivery...");
        vms::streaming::CameraStreamManager::getInstance().stopAll();

        LOG_INFO("Stopping HTTP server...");
        http_server.stop();

        LOG_INFO("Cleaning up managers...");
        camera_manager.cleanup();

        LOG_INFO("Stopping background API workers...");
        vms::api::SynopsisController::shutdown();
        vms::api::ExportController::shutdown();
        vms::events::shutdownDelivery();

        // Final ReID gallery flush — must happen while DbManager is still
        // open. Joins the persistence thread and writes any pending state.
        try {
            vms::core::ReIDEngine::getInstance().shutdown();
        } catch (const std::exception& e) {
            LOG_WARN("ReIDEngine shutdown exception: {}", e.what());
        }

        LOG_INFO("Stopping tracked child processes...");
        vms::core::ProcessManager::getInstance().shutdownAll();

        LOG_INFO("Stopping background storage initialization...");
        vms::utils::StorageManager::getInstance().shutdown();

        // Drain Crow's run() future. http_server.stop() above told the event
        // loop to exit; .get() blocks until the async thread returns and
        // re-throws if run() ended with an exception (which we'd have caught
        // earlier — but logging here is cheap insurance).
        try {
            if (server_future.valid()) {
                server_future.get();
            }
        } catch (const std::exception& e) {
            LOG_WARN("HTTP server future reported exception during shutdown: {}", e.what());
        }

        LOG_INFO("Closing database...");
        db_manager.close();

        // Cleanup global curl
        curl_global_cleanup();

        LOG_INFO("========================================");
        LOG_INFO("Shutdown complete");
        LOG_INFO("========================================");

#ifdef _WIN32
        if (g_shutdown_complete_event) {
            SetEvent(g_shutdown_complete_event);
            CloseHandle(g_shutdown_complete_event);
            g_shutdown_complete_event = nullptr;
        }
#endif
        g_http_server = nullptr;
        g_qt_app = nullptr;
        vms::Logger::shutdown();

#ifdef _WIN32
        timeEndPeriod(1);
#endif

        return result;
        
    } catch (const std::exception& e) {
        // NOTE: cerr is intentional — spdlog may have failed or be shutting down
        std::cerr << "\n[FATAL ERROR] Exception in main: " << e.what() << std::endl;
        LOG_ERROR("Fatal error in main: {}", e.what());
        vms::Logger::shutdown();
        return EXIT_FAILURE;
        
    } catch (...) {
        // NOTE: cerr is intentional — spdlog may have failed or be shutting down
        std::cerr << "\n[FATAL ERROR] Unknown exception in main" << std::endl;
        LOG_ERROR("Unknown fatal error in main");
        vms::Logger::shutdown();
        return EXIT_FAILURE;
    }
}
