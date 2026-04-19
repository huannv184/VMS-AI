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
#include <crtdbg.h>  // For _CrtSetReportMode
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
#include "events/alert_router.h"
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
            std::cerr << "LOG_ERROR: No configuration file specified and none found in default locations!" << std::endl;
            return EXIT_FAILURE;
        }

        std::string absolute_config_path = std::filesystem::absolute(config_path).string();
        std::cout << "Loading configuration from: " << absolute_config_path << std::endl;
        
        if (!vms::Config::getInstance().loadFromFile(config_path)) {
            std::cerr << "LOG_ERROR: Failed to load configuration from: " << config_path << std::endl;
            // The vms::Config::loadFromFile should have printed the specific YAML error
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

        std::thread server_thread([&http_server]() {
            try {
                http_server.run();
            } catch (const std::exception& e) {
                LOG_ERROR("Exception in HTTP server: {}", e.what());
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

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

        LOG_INFO("All managers initialized successfully");

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
        vms::events::AlertRouter::getInstance().shutdown();

        LOG_INFO("Stopping tracked child processes...");
        vms::core::ProcessManager::getInstance().shutdownAll();

        LOG_INFO("Stopping background storage initialization...");
        vms::utils::StorageManager::getInstance().shutdown();

        if (server_thread.joinable()) {
            server_thread.join();
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
