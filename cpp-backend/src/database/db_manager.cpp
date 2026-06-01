#include "database/db_manager.h"
#include "database/db_state.h"
#include "utils/logger.h"
#include <iostream>
#include <filesystem>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <nlohmann/json.hpp>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QThread>
#include <QVariant>
#include <QString>

#include <mutex>

namespace vms {
namespace database {

// Global mutex to serialize access to QSqlDatabase's non-thread-safe connection registry
static std::mutex g_db_conn_mutex;

namespace {

void removeDatabaseConnection(const char* connection_name) {
    std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
    if (!QSqlDatabase::contains(connection_name)) {
        return;
    }

    {
        QSqlDatabase db = QSqlDatabase::database(connection_name, false);
        if (db.isValid() && db.isOpen()) {
            db.close();
        }
    }

    QSqlDatabase::removeDatabase(connection_name);
}

} // namespace

DbManager& DbManager::getInstance() {
    static DbManager instance;
    return instance;
}

DbManager::~DbManager() {
    close();
}

QString DbManager::threadConnectionName() {
    std::ostringstream oss;
    oss << "vms_thread_" << std::this_thread::get_id();
    return QString::fromStdString(oss.str());
}

bool DbManager::init(const Config::DatabaseConfig& config) {
    db_ready.store(false, std::memory_order_release);

    if (initialized_.load(std::memory_order_acquire)) {
        LOG_WARN("Database already initialized");
        db_ready.store(true, std::memory_order_release);
        return true;
    }

    config_ = config;
    removeDatabaseConnection(kPrimaryConnectionName);

    bool initialization_failed = false;
    bool open_failed = false;
    std::string init_error;

    {
        QSqlDatabase primary;

        {
            std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
            if (config_.driver == "postgresql") {
                primary = QSqlDatabase::addDatabase("QPSQL", kPrimaryConnectionName);
                primary.setHostName(QString::fromStdString(config_.postgres.host));
                primary.setPort(config_.postgres.port);
                primary.setDatabaseName(QString::fromStdString(config_.postgres.database));
                primary.setUserName(QString::fromStdString(config_.postgres.username));
                primary.setPassword(QString::fromStdString(config_.postgres.password));
                LOG_INFO("Connecting to PostgreSQL: {}@{}:{}/{}", 
                         config_.postgres.username, config_.postgres.host, 
                         config_.postgres.port, config_.postgres.database);
            } else {
                // Fallback to SQLite
                std::filesystem::path db_path(config_.sqlite.path);
                std::filesystem::path dir = db_path.parent_path();
                if (!dir.empty() && !std::filesystem::exists(dir)) {
                    std::filesystem::create_directories(dir);
                }
                primary = QSqlDatabase::addDatabase("QSQLITE", kPrimaryConnectionName);
                primary.setDatabaseName(QString::fromStdString(config_.sqlite.path));
                LOG_INFO("Connecting to SQLite: {}", config_.sqlite.path);
            }

            if (!primary.open()) {
                open_failed = true;
                initialization_failed = true;
                init_error = primary.lastError().text().toStdString();
            } else {
                LOG_INFO("Primary database connection established successfully");

                // SQLite-specific optimizations
                if (config_.driver == "sqlite") {
                     QSqlQuery wal_query(primary);
                     wal_query.exec("PRAGMA journal_mode=WAL");
                     QSqlQuery timeout_query(primary);
                     timeout_query.exec(QString("PRAGMA busy_timeout=%1").arg(config_.sqlite.busy_timeout_ms));
                }
            }
        }

        if (!initialization_failed) {
            try {
                initializeTables(primary);
            } catch (const std::exception& e) {
                initialization_failed = true;
                init_error = e.what();
            }
        }
    }

    if (initialization_failed) {
        if (open_failed) {
            LOG_ERROR("Can't open database ({}): {}", config_.driver, init_error);
        } else {
            LOG_ERROR("Exception during database initialization: {}", init_error);
        }
        removeDatabaseConnection(kPrimaryConnectionName);
        db_ready.store(false, std::memory_order_release);
        return false;
    }

    // Register primary connection
    {
        std::lock_guard<std::mutex> lock(connection_registry_mutex_);
        registered_connections_.push_back(QString::fromLatin1(kPrimaryConnectionName));
    }

    initialized_.store(true, std::memory_order_release);
    db_ready.store(true, std::memory_order_release);
    startBatchWriter();
    startWalCheckpoint();
    return true;
}

void DbManager::initializeTables(QSqlDatabase& primary) {
    // =========================================================
    // 1. CAMERAS
    // =========================================================
    const std::string sql_cameras = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS cameras (
            id SERIAL PRIMARY KEY,
            name TEXT NOT NULL,
            rtsp_url TEXT NOT NULL,
            location TEXT,
            description TEXT,
            is_active INTEGER DEFAULT 1,
            zmq_port INTEGER DEFAULT 0,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            ai_config TEXT DEFAULT '{"yolo":true,"face":true,"lpr":true,"fire":true}'
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS cameras (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            rtsp_url TEXT NOT NULL,
            location TEXT,
            description TEXT,
            is_active INTEGER DEFAULT 1,
            zmq_port INTEGER DEFAULT 0,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now')),
            ai_config TEXT DEFAULT '{"yolo":true,"face":true,"lpr":true,"fire":true}'
        );
    )";
    if (!executeOnConnection(primary, sql_cameras)) LOG_ERROR("Failed to create cameras table");

    // Camera Migrations
    safeAlterTable(primary, "ALTER TABLE cameras ADD COLUMN sub_stream_url TEXT DEFAULT ''", "sub_stream_url");
    safeAlterTable(primary, "ALTER TABLE cameras ADD COLUMN zmq_port INTEGER DEFAULT 0", "zmq_port");
    safeAlterTable(primary, "ALTER TABLE cameras ADD COLUMN ai_config TEXT DEFAULT '{\"yolo\":true,\"face\":true,\"lpr\":true,\"fire\":true}'", "ai_config");
    safeAlterTable(primary, "ALTER TABLE cameras ADD COLUMN advanced_config TEXT", "advanced_config");

    // =========================================================
    // 2. ROIS
    // =========================================================
    const std::string sql_rois = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS rois (
            id SERIAL PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            roi_type TEXT NOT NULL,
            points_json TEXT NOT NULL,
            is_active INTEGER DEFAULT 1,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            CONSTRAINT fk_camera FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS rois (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            camera_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            roi_type TEXT NOT NULL,
            points_json TEXT NOT NULL,
            is_active INTEGER DEFAULT 1,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE
        );
    )";
    if (!executeOnConnection(primary, sql_rois)) LOG_ERROR("Failed to create rois table");

    // =========================================================
    // 3. EVENTS
    // =========================================================
    const std::string sql_events = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS events (
            id TEXT PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            event_type TEXT NOT NULL,
            title TEXT,
            description TEXT,
            severity TEXT,
            snapshot_path TEXT,
            image_path TEXT,
            timestamp BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            metadata_json TEXT,
            details_json TEXT,
            is_read INTEGER DEFAULT 0,
            video_path TEXT,
            duration INTEGER DEFAULT 0,
            CONSTRAINT fk_camera FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS events (
            id TEXT PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            event_type TEXT NOT NULL,
            title TEXT,
            description TEXT,
            severity TEXT,
            snapshot_path TEXT,
            image_path TEXT,
            timestamp INTEGER DEFAULT (strftime('%s', 'now')),
            metadata_json TEXT,
            details_json TEXT,
            is_read INTEGER DEFAULT 0,
            FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE
        );
    )";
    if (!executeOnConnection(primary, sql_events)) LOG_ERROR("Failed to create events table");

    // Event Migrations
    safeAlterTable(primary, "ALTER TABLE events ADD COLUMN video_path TEXT", "video_path");
    safeAlterTable(primary, "ALTER TABLE events ADD COLUMN duration INTEGER DEFAULT 0", "duration");
    safeAlterTable(primary, "ALTER TABLE events ADD COLUMN title TEXT", "title");
    safeAlterTable(primary, "ALTER TABLE events ADD COLUMN description TEXT", "description");
    safeAlterTable(primary, "ALTER TABLE events ADD COLUMN snapshot_path TEXT", "snapshot_path");
    safeAlterTable(primary, "ALTER TABLE events ADD COLUMN metadata_json TEXT", "metadata_json");

    // =========================================================
    // 4. PERSONS (Face Database)
    // =========================================================
    const std::string sql_persons = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS persons (
            id SERIAL PRIMARY KEY,
            name TEXT NOT NULL,
            embedding_blob BYTEA,
            embedding_json TEXT,
            face_image_path TEXT,
            description TEXT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS persons (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            embedding_blob BLOB,
            embedding_json TEXT,
            face_image_path TEXT,
            description TEXT,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_persons)) LOG_ERROR("Failed to create persons table");

    // Persons Migrations
    safeAlterTable(primary, "ALTER TABLE persons ADD COLUMN description TEXT", "description");
    safeAlterTable(primary, "ALTER TABLE persons ADD COLUMN embedding_json TEXT", "embedding_json");
    safeAlterTable(primary, "ALTER TABLE persons ADD COLUMN face_image_path TEXT", "face_image_path");

    // =========================================================
    // 5. LICENSE PLATES
    // =========================================================
    const std::string sql_license_plates = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS license_plates (
            id SERIAL PRIMARY KEY,
            plate_number TEXT NOT NULL,
            vehicle_type TEXT DEFAULT 'unknown',
            color TEXT,
            image_path TEXT,
            camera_id INTEGER,
            confidence REAL DEFAULT 0.0,
            detected_at BIGINT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        )
    )" : R"(
        CREATE TABLE IF NOT EXISTS license_plates (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plate_number TEXT NOT NULL,
            vehicle_type TEXT DEFAULT 'unknown',
            color TEXT,
            image_path TEXT,
            camera_id INTEGER,
            confidence REAL DEFAULT 0.0,
            detected_at INTEGER,
            created_at INTEGER DEFAULT (strftime('%s', 'now'))
        )
    )";
    if (!executeOnConnection(primary, sql_license_plates)) LOG_ERROR("Failed to create license_plates table");

    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_license_plates_number ON license_plates(plate_number)");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_license_plates_detected_at ON license_plates(detected_at)");

    // =========================================================
    // 5.5. VEHICLES (Registered vehicles)
    // =========================================================
    const std::string sql_vehicles = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS vehicles (
            id SERIAL PRIMARY KEY,
            plate_number TEXT UNIQUE NOT NULL,
            owner_name TEXT,
            brand TEXT,
            color TEXT,
            vehicle_type TEXT,
            description TEXT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS vehicles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plate_number TEXT UNIQUE NOT NULL,
            owner_name TEXT,
            brand TEXT,
            color TEXT,
            vehicle_type TEXT,
            description TEXT,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_vehicles)) LOG_ERROR("Failed to create vehicles table");

    // =========================================================
    // 5.7. TRAFFIC COUNTS (Phase 1 AI – vehicle counting)
    // =========================================================
    const std::string sql_traffic_counts = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS traffic_counts (
            id           SERIAL PRIMARY KEY,
            camera_id    INTEGER NOT NULL,
            roi_id       INTEGER DEFAULT -1,
            direction    TEXT    DEFAULT 'both',
            vehicle_type TEXT    DEFAULT 'all',
            count        INTEGER DEFAULT 0,
            period_start BIGINT NOT NULL,
            period_end   BIGINT NOT NULL,
            created_at   BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            CONSTRAINT fk_camera FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS traffic_counts (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            camera_id    INTEGER NOT NULL,
            roi_id       INTEGER DEFAULT -1,
            direction    TEXT    DEFAULT 'both',
            vehicle_type TEXT    DEFAULT 'all',
            count        INTEGER DEFAULT 0,
            period_start INTEGER NOT NULL,
            period_end   INTEGER NOT NULL,
            created_at   INTEGER DEFAULT (strftime('%s', 'now')),
            FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE
        );
    )";
    if (!executeOnConnection(primary, sql_traffic_counts)) LOG_ERROR("Failed to create traffic_counts table");

    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_traffic_camera_time ON traffic_counts(camera_id, period_start)");

    // =========================================================
    // 5.8. AUDIT LOGS (Phase 4 Security & Control)
    // =========================================================
    const std::string sql_audit_logs = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS audit_logs (
            id SERIAL PRIMARY KEY,
            user_id INTEGER,
            action TEXT NOT NULL,
            details TEXT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS audit_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER,
            action TEXT NOT NULL,
            details TEXT,
            created_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_audit_logs)) LOG_ERROR("Failed to create audit_logs table");

    // =========================================================
    // 6. ROLES
    // =========================================================
    const std::string sql_roles = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS roles (
            id SERIAL PRIMARY KEY,
            name TEXT UNIQUE NOT NULL,
            permissions TEXT NOT NULL,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS roles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE NOT NULL,
            permissions TEXT NOT NULL,
            created_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_roles)) LOG_ERROR("Failed to create roles table");

    if (config_.driver == "postgresql") {
        executeOnConnection(primary, "INSERT INTO roles (name, permissions) VALUES ('admin', '[\"all\"]') ON CONFLICT (name) DO NOTHING");
        executeOnConnection(primary, "INSERT INTO roles (name, permissions) VALUES ('operator', '[\"camera.view\", \"events.view\", \"events.manage\"]') ON CONFLICT (name) DO NOTHING");
        executeOnConnection(primary, "INSERT INTO roles (name, permissions) VALUES ('viewer', '[\"camera.view\"]') ON CONFLICT (name) DO NOTHING");
    } else {
        executeOnConnection(primary, "INSERT OR IGNORE INTO roles (name, permissions) VALUES ('admin', '[\"all\"]')");
        executeOnConnection(primary, "INSERT OR IGNORE INTO roles (name, permissions) VALUES ('operator', '[\"camera.view\", \"events.view\", \"events.manage\"]')");
        executeOnConnection(primary, "INSERT OR IGNORE INTO roles (name, permissions) VALUES ('viewer', '[\"camera.view\"]')");
    }

    // =========================================================
    // 7. USERS
    // =========================================================
    const std::string sql_users = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS users (
            id SERIAL PRIMARY KEY,
            username TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL,
            role_id INTEGER,
            full_name TEXT,
            email TEXT,
            is_active INTEGER DEFAULT 1,
            last_login BIGINT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            CONSTRAINT fk_role FOREIGN KEY(role_id) REFERENCES roles(id)
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL,
            role_id INTEGER,
            full_name TEXT,
            email TEXT,
            is_active INTEGER DEFAULT 1,
            last_login INTEGER,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            FOREIGN KEY(role_id) REFERENCES roles(id)
        );
    )";
    if (!executeOnConnection(primary, sql_users)) LOG_ERROR("Failed to create users table");

    if (config_.driver == "postgresql") {
        executeOnConnection(primary,
            "INSERT INTO users (username, password_hash, role_id, full_name, is_active) "
            "VALUES ('admin', '8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918', 1, 'System Administrator', 1) "
            "ON CONFLICT (username) DO NOTHING");
    } else {
        executeOnConnection(primary,
            "INSERT OR IGNORE INTO users (username, password_hash, role_id, full_name, is_active) "
            "VALUES ('admin', '8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918', 1, 'System Administrator', 1)");
    }

    // SEC-001: Detect default admin password and flag it.
    // If the admin account still uses SHA256("admin"), set the global
    // default_password_active flag so the login endpoint can enforce a
    // mandatory password change.
    {
        QSqlQuery check_query(primary);
        check_query.prepare("SELECT password_hash FROM users WHERE username='admin'");
        if (check_query.exec() && check_query.next()) {
            QString hash = check_query.value(0).toString();
            if (hash == "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918") {
                vms::database::default_password_active.store(true, std::memory_order_release);
                LOG_CRITICAL("==================================================");
                LOG_CRITICAL("SEC-001 CRITICAL: Default admin password detected!");
                LOG_CRITICAL("The admin account is using the factory-default password.");
                LOG_CRITICAL("Login will require an immediate password change.");
                LOG_CRITICAL("==================================================");
            } else {
                vms::database::default_password_active.store(false, std::memory_order_release);
            }
        }
    }

    // User Migrations
    safeAlterTable(primary, "ALTER TABLE users ADD COLUMN salt TEXT DEFAULT ''", "users.salt");
    safeAlterTable(primary, "ALTER TABLE users ADD COLUMN two_factor_enabled INTEGER DEFAULT 0", "users.two_factor_enabled");
    safeAlterTable(primary, "ALTER TABLE users ADD COLUMN two_factor_secret TEXT DEFAULT ''", "users.two_factor_secret");
    // H2: token_version enables instant JWT invalidation without waiting for expiry.
    // Increment this column to invalidate all existing tokens for a user (deactivation, role change, forced logout).
    safeAlterTable(primary, "ALTER TABLE users ADD COLUMN token_version INTEGER NOT NULL DEFAULT 0", "users.token_version");

    // =========================================================
    // 8. INDEXES
    // =========================================================
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_events_camera_time ON events(camera_id, timestamp)");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_events_unread ON events(is_read)");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_events_type ON events(event_type)");

    // =========================================================
    // 8.5. ZONES & RULES (Advanced Event Engine)
    // =========================================================
    const std::string sql_zones = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS zones (
            id SERIAL PRIMARY KEY,
            name TEXT NOT NULL,
            type TEXT DEFAULT 'public',
            description TEXT,
            polygon_json TEXT NOT NULL,
            camera_ids_json TEXT NOT NULL,
            max_occupancy INTEGER DEFAULT -1,
            allow_loitering INTEGER DEFAULT 1,
            loitering_threshold_sec INTEGER DEFAULT 30,
            enable_alerts INTEGER DEFAULT 1,
            max_alerts_per_minute INTEGER DEFAULT 5,
            metadata_json TEXT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS zones (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            type TEXT DEFAULT 'public',
            description TEXT,
            polygon_json TEXT NOT NULL,
            camera_ids_json TEXT NOT NULL,
            max_occupancy INTEGER DEFAULT -1,
            allow_loitering INTEGER DEFAULT 1,
            loitering_threshold_sec INTEGER DEFAULT 30,
            enable_alerts INTEGER DEFAULT 1,
            max_alerts_per_minute INTEGER DEFAULT 5,
            metadata_json TEXT,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_zones)) LOG_ERROR("Failed to create zones table");

    const std::string sql_rules = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS rules (
            id SERIAL PRIMARY KEY,
            name TEXT NOT NULL,
            description TEXT,
            enabled INTEGER DEFAULT 1,
            logic TEXT DEFAULT 'AND',
            conditions_json TEXT NOT NULL,
            actions_json TEXT NOT NULL,
            anti_noise_json TEXT NOT NULL,
            camera_ids_json TEXT NOT NULL,
            metadata_json TEXT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS rules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT,
            enabled INTEGER DEFAULT 1,
            logic TEXT DEFAULT 'AND',
            conditions_json TEXT NOT NULL,
            actions_json TEXT NOT NULL,
            anti_noise_json TEXT NOT NULL,
            camera_ids_json TEXT NOT NULL,
            metadata_json TEXT,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_rules)) LOG_ERROR("Failed to create rules table");

    // =========================================================
    // 9. SETTINGS
    // =========================================================
    const std::string sql_settings = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL,
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL,
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_settings)) LOG_ERROR("Failed to create settings table");

    // =========================================================
    // 10. RECORDING SEGMENTS (Continuous 24/7 recording)
    // =========================================================
    const std::string sql_recording_segments = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS recording_segments (
            id SERIAL PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            filename TEXT NOT NULL,
            start_time BIGINT NOT NULL,
            end_time BIGINT NOT NULL,
            file_size BIGINT DEFAULT 0,
            status TEXT DEFAULT 'completed',
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            CONSTRAINT fk_camera FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS recording_segments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            camera_id INTEGER NOT NULL,
            filename TEXT NOT NULL,
            start_time INTEGER NOT NULL,
            end_time INTEGER NOT NULL,
            file_size INTEGER DEFAULT 0,
            status TEXT DEFAULT 'completed',
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE
        );
    )";
    if (!executeOnConnection(primary, sql_recording_segments)) LOG_ERROR("Failed to create recording_segments table");

    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_segments_camera_time ON recording_segments(camera_id, start_time)");

    // =========================================================
    // 11. DEVICES (NVR/DVR/Encoder management)
    // =========================================================
    const std::string sql_devices = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS devices (
            id SERIAL PRIMARY KEY,
            name TEXT NOT NULL,
            type TEXT NOT NULL DEFAULT 'NVR',
            ip_address TEXT NOT NULL,
            port INTEGER DEFAULT 80,
            brand TEXT,
            model TEXT,
            firmware TEXT,
            username TEXT,
            password TEXT,
            channel_count INTEGER DEFAULT 0,
            is_online INTEGER DEFAULT 0,
            last_seen BIGINT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS devices (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            type TEXT NOT NULL DEFAULT 'NVR',
            ip_address TEXT NOT NULL,
            port INTEGER DEFAULT 80,
            brand TEXT,
            model TEXT,
            firmware TEXT,
            username TEXT,
            password TEXT,
            channel_count INTEGER DEFAULT 0,
            is_online INTEGER DEFAULT 0,
            last_seen INTEGER,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_devices)) LOG_ERROR("Failed to create devices table");

    // Camera Migrations — Motion Detection
    safeAlterTable(primary, "ALTER TABLE cameras ADD COLUMN motion_detection_enabled INTEGER DEFAULT 0", "motion_detection_enabled");
    safeAlterTable(primary, "ALTER TABLE cameras ADD COLUMN motion_sensitivity REAL DEFAULT 0.02", "motion_sensitivity");

    // =========================================================
    // 12. MULTI-SITE SUPPORT
    // =========================================================
    const std::string sql_sites = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS sites (
            id SERIAL PRIMARY KEY,
            name TEXT NOT NULL,
            address TEXT,
            parent_site_id INTEGER DEFAULT -1,
            description TEXT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS sites (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            address TEXT,
            parent_site_id INTEGER DEFAULT -1,
            description TEXT,
            created_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_sites)) LOG_ERROR("Failed to create sites table");

    // Camera Migration for Site ID
    safeAlterTable(primary, "ALTER TABLE cameras ADD COLUMN site_id INTEGER DEFAULT -1", "cameras.site_id");

    // =========================================================
    // 13. USER PERMISSIONS (Multi-site RBAC)
    // =========================================================
    const std::string sql_permissions = R"(
        CREATE TABLE IF NOT EXISTS permissions (
            user_id INTEGER PRIMARY KEY,
            allowed_cameras TEXT DEFAULT '[]',
            allowed_sites TEXT DEFAULT '[]',
            can_view_live INTEGER DEFAULT 1,
            can_view_playback INTEGER DEFAULT 1,
            can_manage_settings INTEGER DEFAULT 0,
            can_ptz INTEGER DEFAULT 1,
            FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
        );
    )";
    if (!executeOnConnection(primary, sql_permissions)) LOG_ERROR("Failed to create permissions table");

    // =========================================================
    // 14. VIDEO WALL LAYOUTS
    // =========================================================
    const std::string sql_videowall_layouts = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS videowall_layouts (
            id SERIAL PRIMARY KEY,
            name TEXT NOT NULL,
            grid_cols INTEGER DEFAULT 4,
            grid_rows INTEGER DEFAULT 4,
            cells_json TEXT DEFAULT '[]',
            created_by TEXT DEFAULT 'admin',
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS videowall_layouts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            grid_cols INTEGER DEFAULT 4,
            grid_rows INTEGER DEFAULT 4,
            cells_json TEXT DEFAULT '[]',
            created_by TEXT DEFAULT 'admin',
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_videowall_layouts)) LOG_ERROR("Failed to create videowall_layouts table");

    // =========================================================
    // 15. ALERT RULES (Cấu hình cảnh báo)
    // =========================================================
    const std::string sql_alert_rules = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS alert_rules (
            id SERIAL PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            event_type TEXT NOT NULL,
            severity TEXT NOT NULL DEFAULT 'high',
            action_type TEXT NOT NULL, -- email, webhook, sms
            recipient TEXT,
            is_enabled INTEGER DEFAULT 1,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS alert_rules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            camera_id INTEGER NOT NULL,
            event_type TEXT NOT NULL,
            severity TEXT NOT NULL DEFAULT 'high',
            action_type TEXT NOT NULL, -- email, webhook, sms
            recipient TEXT,
            is_enabled INTEGER DEFAULT 1,
            created_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    if (!executeOnConnection(primary, sql_alert_rules)) LOG_ERROR("Failed to create alert_rules table");

    // =========================================================
    // 16. SHIFTS (Ca làm việc cho module chấm công)
    // =========================================================
    const std::string sql_shifts = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS shifts (
            id SERIAL PRIMARY KEY,
            name TEXT NOT NULL UNIQUE,
            start_time_hm TEXT NOT NULL,           -- 'HH:MM' giờ bắt đầu ca
            end_time_hm TEXT NOT NULL,             -- 'HH:MM' giờ kết thúc ca
            late_threshold_min INTEGER NOT NULL DEFAULT 15,
            grace_min INTEGER NOT NULL DEFAULT 0,
            ot_grace_min INTEGER NOT NULL DEFAULT 0,    -- minutes after end before OT starts
            ot_min_minutes INTEGER NOT NULL DEFAULT 0,  -- minimum OT to qualify (skip <N minutes of trailing presence)
            ot_max_minutes INTEGER NOT NULL DEFAULT 720,-- cap to filter outliers (default 12h)
            active INTEGER NOT NULL DEFAULT 1,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS shifts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            start_time_hm TEXT NOT NULL,
            end_time_hm TEXT NOT NULL,
            late_threshold_min INTEGER NOT NULL DEFAULT 15,
            grace_min INTEGER NOT NULL DEFAULT 0,
            ot_grace_min INTEGER NOT NULL DEFAULT 0,
            ot_min_minutes INTEGER NOT NULL DEFAULT 0,
            ot_max_minutes INTEGER NOT NULL DEFAULT 720,
            active INTEGER NOT NULL DEFAULT 1,
            created_at INTEGER DEFAULT (strftime('%s','now')),
            updated_at INTEGER DEFAULT (strftime('%s','now'))
        );
    )";
    if (!executeOnConnection(primary, sql_shifts)) LOG_ERROR("Failed to create shifts table");
    // Migration for sites that already have the table from before OT
    // columns existed (2026-05-04).  safeAlterTable swallows "column
    // already exists" so this is idempotent.
    safeAlterTable(primary, "ALTER TABLE shifts ADD COLUMN ot_grace_min INTEGER NOT NULL DEFAULT 0", "shifts.ot_grace_min");
    safeAlterTable(primary, "ALTER TABLE shifts ADD COLUMN ot_min_minutes INTEGER NOT NULL DEFAULT 0", "shifts.ot_min_minutes");
    safeAlterTable(primary, "ALTER TABLE shifts ADD COLUMN ot_max_minutes INTEGER NOT NULL DEFAULT 720", "shifts.ot_max_minutes");

    // =========================================================
    // 16b. HOLIDAYS (Lịch nghỉ — tắt tính trễ cho ngày trong bảng)
    //
    // Per-date holiday calendar. queryAttendanceForDate looks up the row for
    // the requested date and, when present, sets is_holiday=true on every
    // employee's rollup + skips late_minutes / OT computation (presence on a
    // holiday is recorded but doesn't count against the operator's late
    // metric). Punted from the 2026-05-03 shifts pass; landed 2026-05-12.
    //
    // V1: exact-date matches only (one row per holiday per year). Recurring
    // annual holidays (e.g. Tết Dương lịch 1/1) get duplicated rows each
    // year — operators pre-load the next year's calendar in advance. A
    // future `recurring` flag matching by MM-DD is documented in the
    // holidays_calendar memory note.
    // =========================================================
    const std::string sql_holidays = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS holidays (
            id SERIAL PRIMARY KEY,
            date TEXT NOT NULL UNIQUE,            -- 'YYYY-MM-DD'
            name TEXT NOT NULL,                   -- 'Tết Dương lịch', 'Quốc khánh', ...
            description TEXT,                     -- optional notes / nghị định ref
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS holidays (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            date TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            description TEXT,
            created_at INTEGER DEFAULT (strftime('%s','now'))
        );
    )";
    if (!executeOnConnection(primary, sql_holidays)) LOG_ERROR("Failed to create holidays table");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_holidays_date ON holidays(date)");

    // =========================================================
    // 17. EMPLOYEES (Liên kết người trong face DB → mã nhân viên + ca)
    // =========================================================
    const std::string sql_employees = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS employees (
            id SERIAL PRIMARY KEY,
            person_id INTEGER NOT NULL UNIQUE,     -- FK -> persons(id) (face DB)
            code TEXT UNIQUE,                      -- Mã NV (vd EMP001)
            full_name TEXT,
            dept TEXT,
            shift_id INTEGER,                      -- FK -> shifts(id)
            active INTEGER NOT NULL DEFAULT 1,
            hired_at BIGINT,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS employees (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            person_id INTEGER NOT NULL UNIQUE,
            code TEXT UNIQUE,
            full_name TEXT,
            dept TEXT,
            shift_id INTEGER,
            active INTEGER NOT NULL DEFAULT 1,
            hired_at INTEGER,
            created_at INTEGER DEFAULT (strftime('%s','now')),
            updated_at INTEGER DEFAULT (strftime('%s','now'))
        );
    )";
    if (!executeOnConnection(primary, sql_employees)) LOG_ERROR("Failed to create employees table");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_employees_person ON employees(person_id)");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_employees_dept ON employees(dept)");

    // =========================================================
    // 18. ATTENDANCE_EVENTS (Raw log từ AttendanceTracker, đã dedup ≥60s)
    // =========================================================
    const std::string sql_attendance_events = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS attendance_events (
            id BIGSERIAL PRIMARY KEY,
            person_id INTEGER NOT NULL,            -- persons(id), -1 nếu chưa nhận diện
            employee_id INTEGER,                   -- employees(id), nullable nếu chưa link
            camera_id INTEGER NOT NULL,
            kind TEXT NOT NULL CHECK (kind IN ('in','out','seen')),
            timestamp BIGINT NOT NULL,             -- epoch giây
            confidence REAL,
            snapshot_path TEXT,
            source_rule TEXT,                      -- 'door_role' | 'min_max_fallback' | 'manual'
            metadata_json TEXT
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS attendance_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            person_id INTEGER NOT NULL,
            employee_id INTEGER,
            camera_id INTEGER NOT NULL,
            kind TEXT NOT NULL CHECK (kind IN ('in','out','seen')),
            timestamp INTEGER NOT NULL,
            confidence REAL,
            snapshot_path TEXT,
            source_rule TEXT,
            metadata_json TEXT
        );
    )";
    if (!executeOnConnection(primary, sql_attendance_events)) LOG_ERROR("Failed to create attendance_events table");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_att_person_ts ON attendance_events(person_id, timestamp)");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_att_ts ON attendance_events(timestamp)");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_att_emp_ts ON attendance_events(employee_id, timestamp)");

    // =========================================================
    // 19. CAMERA_ROLES (Vai trò vào/ra của camera)
    // =========================================================
    const std::string sql_camera_roles = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS camera_roles (
            camera_id INTEGER PRIMARY KEY,
            role TEXT NOT NULL CHECK (role IN ('entry','exit','both','observe')),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS camera_roles (
            camera_id INTEGER PRIMARY KEY,
            role TEXT NOT NULL CHECK (role IN ('entry','exit','both','observe')),
            updated_at INTEGER DEFAULT (strftime('%s','now'))
        );
    )";
    if (!executeOnConnection(primary, sql_camera_roles)) LOG_ERROR("Failed to create camera_roles table");

    // =========================================================
    // 20. COUNTING_LINES (Đường ảo cho module đếm người)
    // =========================================================
    const std::string sql_counting_lines = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS counting_lines (
            id SERIAL PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            name TEXT,
            ax REAL NOT NULL,
            ay REAL NOT NULL,
            bx REAL NOT NULL,
            by REAL NOT NULL,
            direction_a_label TEXT DEFAULT 'in',
            direction_b_label TEXT DEFAULT 'out',
            object_classes_json TEXT DEFAULT '["person"]',
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW())),
            updated_at BIGINT DEFAULT (EXTRACT(EPOCH FROM NOW()))
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS counting_lines (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            camera_id INTEGER NOT NULL,
            name TEXT,
            ax REAL NOT NULL,
            ay REAL NOT NULL,
            bx REAL NOT NULL,
            by REAL NOT NULL,
            direction_a_label TEXT DEFAULT 'in',
            direction_b_label TEXT DEFAULT 'out',
            object_classes_json TEXT DEFAULT '["person"]',
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at INTEGER DEFAULT (strftime('%s','now')),
            updated_at INTEGER DEFAULT (strftime('%s','now'))
        );
    )";
    if (!executeOnConnection(primary, sql_counting_lines)) LOG_ERROR("Failed to create counting_lines table");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_counting_lines_cam ON counting_lines(camera_id)");

    // =========================================================
    // 21. COUNTER_BUCKETS_1M (Bucket đếm 1 phút cho timeseries)
    // =========================================================
    const std::string sql_counter_buckets = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS counter_buckets_1m (
            id BIGSERIAL PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            source_kind TEXT NOT NULL CHECK (source_kind IN ('line','zone')),
            source_id INTEGER NOT NULL,
            ts_minute BIGINT NOT NULL,             -- epoch giây của phút (đã round-down)
            in_count INTEGER NOT NULL DEFAULT 0,
            out_count INTEGER NOT NULL DEFAULT 0,
            occupancy_max INTEGER NOT NULL DEFAULT 0,
            occupancy_avg REAL NOT NULL DEFAULT 0,
            UNIQUE (camera_id, source_kind, source_id, ts_minute)
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS counter_buckets_1m (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            camera_id INTEGER NOT NULL,
            source_kind TEXT NOT NULL CHECK (source_kind IN ('line','zone')),
            source_id INTEGER NOT NULL,
            ts_minute INTEGER NOT NULL,
            in_count INTEGER NOT NULL DEFAULT 0,
            out_count INTEGER NOT NULL DEFAULT 0,
            occupancy_max INTEGER NOT NULL DEFAULT 0,
            occupancy_avg REAL NOT NULL DEFAULT 0,
            UNIQUE (camera_id, source_kind, source_id, ts_minute)
        );
    )";
    if (!executeOnConnection(primary, sql_counter_buckets)) LOG_ERROR("Failed to create counter_buckets_1m table");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_buckets_ts ON counter_buckets_1m(ts_minute)");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_buckets_src ON counter_buckets_1m(camera_id, source_kind, source_id, ts_minute)");

    // =========================================================
    // 22. REID_GALLERY (Cross-camera person Re-ID gallery — persistence)
    //
    // Pre-2026-05-14 the ReIDEngine gallery was in-memory only — every
    // backend restart wiped operator-built person identities. Persist the
    // gallery + per-person trail rows so a process bounce or NSSM service
    // restart doesn't cold-start identity assignment.
    // Embedding is stored as raw float32 BLOB (4 bytes per dim × embedding_dim).
    // =========================================================
    const std::string sql_reid_gallery = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS reid_gallery (
            global_id INTEGER PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            track_id INTEGER NOT NULL,
            embedding BYTEA NOT NULL,
            embedding_dim INTEGER NOT NULL,
            thumbnail_path TEXT,
            first_seen BIGINT NOT NULL,
            last_seen BIGINT NOT NULL
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS reid_gallery (
            global_id INTEGER PRIMARY KEY,
            camera_id INTEGER NOT NULL,
            track_id INTEGER NOT NULL,
            embedding BLOB NOT NULL,
            embedding_dim INTEGER NOT NULL,
            thumbnail_path TEXT,
            first_seen INTEGER NOT NULL,
            last_seen INTEGER NOT NULL
        );
    )";
    if (!executeOnConnection(primary, sql_reid_gallery)) LOG_ERROR("Failed to create reid_gallery table");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_reid_last_seen ON reid_gallery(last_seen)");

    const std::string sql_reid_trails = (config_.driver == "postgresql") ? R"(
        CREATE TABLE IF NOT EXISTS reid_trails (
            id BIGSERIAL PRIMARY KEY,
            global_id INTEGER NOT NULL,
            camera_id INTEGER NOT NULL,
            thumbnail_path TEXT,
            enter_time BIGINT NOT NULL,
            exit_time BIGINT NOT NULL
        );
    )" : R"(
        CREATE TABLE IF NOT EXISTS reid_trails (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            global_id INTEGER NOT NULL,
            camera_id INTEGER NOT NULL,
            thumbnail_path TEXT,
            enter_time INTEGER NOT NULL,
            exit_time INTEGER NOT NULL
        );
    )";
    if (!executeOnConnection(primary, sql_reid_trails)) LOG_ERROR("Failed to create reid_trails table");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_reid_trails_gid ON reid_trails(global_id)");
    executeOnConnection(primary, "CREATE INDEX IF NOT EXISTS idx_reid_trails_enter ON reid_trails(enter_time)");

    LOG_INFO("All database tables initialized successfully");
}

void DbManager::close() {
    // Join wal-checkpoint thread BEFORE batch writer + connection teardown so
    // it cannot deref a torn-down per-thread QSqlDatabase mid-PRAGMA.
    stopWalCheckpoint();
    stopBatchWriter();

    initialized_.store(false, std::memory_order_release);
    db_ready.store(false, std::memory_order_release);

    // Close and remove all registered connections
    {
        std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
        std::lock_guard<std::mutex> lock(connection_registry_mutex_);
        for (const auto& conn_name : registered_connections_) {
            {
                QSqlDatabase db = QSqlDatabase::database(conn_name, false);
                if (db.isOpen()) {
                    db.close();
                }
            }
            QSqlDatabase::removeDatabase(conn_name);
        }
        registered_connections_.clear();

        // Also remove the primary connection if not already removed
        if (QSqlDatabase::contains(kPrimaryConnectionName)) {
            {
                QSqlDatabase db = QSqlDatabase::database(kPrimaryConnectionName, false);
                if (db.isOpen()) {
                    db.close();
                }
            }
            QSqlDatabase::removeDatabase(kPrimaryConnectionName);
        }
    }

    LOG_INFO("Database closed");
}

QSqlDatabase DbManager::getThreadConnection() {
    if (!initialized_.load(std::memory_order_acquire)) {
        return QSqlDatabase();
    }

    QString conn_name = threadConnectionName();

    QSqlDatabase db;
    {
        std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
        if (QSqlDatabase::contains(conn_name)) {
            db = QSqlDatabase::database(conn_name, false);
            if (db.isOpen()) {
                return db;
            }
            // Connection exists but is closed — remove and recreate
            QSqlDatabase::removeDatabase(conn_name);
        }

        // Create a new connection for this thread
        if (config_.driver == "postgresql") {
            db = QSqlDatabase::addDatabase("QPSQL", conn_name);
            db.setHostName(QString::fromStdString(config_.postgres.host));
            db.setPort(config_.postgres.port);
            db.setDatabaseName(QString::fromStdString(config_.postgres.database));
            db.setUserName(QString::fromStdString(config_.postgres.username));
            db.setPassword(QString::fromStdString(config_.postgres.password));
        } else {
            db = QSqlDatabase::addDatabase("QSQLITE", conn_name);
            db.setDatabaseName(QString::fromStdString(config_.sqlite.path));
        }
    } // Release g_db_conn_mutex before opening

    if (!db.open()) {
        LOG_THROTTLED_ERROR(5000, "Failed to open thread-local database connection: {}",
                  db.lastError().text().toStdString());
        std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
        QSqlDatabase::removeDatabase(conn_name);
        return QSqlDatabase();
    }

    // SQLite-specific configuration
    if (config_.driver == "sqlite") {
        QSqlQuery wal_query(db);
        wal_query.exec("PRAGMA journal_mode=WAL");
        QSqlQuery timeout_query(db);
        timeout_query.exec(QString("PRAGMA busy_timeout=%1").arg(config_.sqlite.busy_timeout_ms));
    }

    // Register for cleanup
    {
        std::lock_guard<std::mutex> lock(connection_registry_mutex_);
        registered_connections_.push_back(conn_name);
    }

    return db;
}

bool DbManager::execute(const std::string& sql) {
    if (!initialized_.load(std::memory_order_acquire)) {
        LOG_THROTTLED_WARN(5000, "Database not initialized — query dropped");
        return false;
    }

    QSqlDatabase db = getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_THROTTLED_ERROR(5000, "Failed to get thread connection for execute");
        return false;
    }

    return executeOnConnection(db, sql);
}

bool DbManager::executeOnConnection(QSqlDatabase& db, const std::string& sql) {
    QSqlQuery query(db);
    if (!query.exec(QString::fromStdString(sql))) {
        LOG_ERROR("SQL error: {}", query.lastError().text().toStdString());
        return false;
    }
    return true;
}

void DbManager::safeAlterTable(QSqlDatabase& db, const std::string& sql, const std::string& column_name) {
    QSqlQuery query(db);
    if (!query.exec(QString::fromStdString(sql))) {
        std::string err = query.lastError().text().toStdString();
        if (err.find("duplicate column") == std::string::npos &&
            err.find("duplicate column name") == std::string::npos) {
            LOG_WARN("Migration failed ({}): {}", column_name, err);
        }
    }
}

bool DbManager::executeParameterized(const std::string& sql, const std::vector<std::string>& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        LOG_THROTTLED_WARN(5000, "Database not initialized — parameterized query dropped");
        return false;
    }

    QSqlDatabase db = getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_THROTTLED_ERROR(5000, "Failed to get thread connection for executeParameterized");
        return false;
    }

    QSqlQuery query(db);
    if (!query.prepare(QString::fromStdString(sql))) {
        LOG_ERROR("Failed to prepare statement: {}", query.lastError().text().toStdString());
        return false;
    }

    for (size_t i = 0; i < params.size(); ++i) {
        query.bindValue(static_cast<int>(i), QString::fromStdString(params[i]));
    }

    if (!query.exec()) {
        LOG_ERROR("Failed to execute parameterized query: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

// 2026-05-19 transaction_mutex_ removed (was DB-004 vestige).
//
// Each thread has its own QSqlDatabase via getThreadConnection() — keyed
// on std::this_thread::get_id() — so two threads cannot share a
// connection and SQL-level transactions are isolated per-connection by
// definition (Qt's QSqlDatabase::transaction()/commit()/rollback() act
// on the per-connection driver). The mutex was a global app-level
// serializer that cost (a) needless serialization of zone/rule/reid
// saves with no SQL conflict, and (b) catastrophic deadlock if any
// code path threw between begin and commit/rollback without unlock.
//
// Same-thread nested begin pre-fix → deadlock on std::mutex; post-fix
// → db.transaction() returns false (Qt rejects double-begin on the
// same connection) → LOG_ERROR + caller can recover. Strictly better.
void DbManager::beginTransaction() {
    if (!initialized_.load(std::memory_order_acquire)) {
        LOG_THROTTLED_WARN(5000, "beginTransaction: Database not initialized");
        return;
    }

    QSqlDatabase db = getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_THROTTLED_ERROR(5000, "Failed to get thread connection for beginTransaction");
        return;
    }

    if (!db.transaction()) {
        LOG_ERROR("Failed to begin transaction: {}", db.lastError().text().toStdString());
    }
}

void DbManager::commit() {
    if (!initialized_.load(std::memory_order_acquire)) {
        LOG_THROTTLED_WARN(5000, "commit: Database not initialized");
        return;
    }

    QSqlDatabase db = getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_THROTTLED_ERROR(5000, "Failed to get thread connection for commit");
        return;
    }

    if (!db.commit()) {
        LOG_ERROR("Failed to commit transaction: {}", db.lastError().text().toStdString());
    }
}

void DbManager::rollback() {
    if (!initialized_.load(std::memory_order_acquire)) {
        LOG_THROTTLED_WARN(5000, "rollback: Database not initialized");
        return;
    }

    QSqlDatabase db = getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_THROTTLED_ERROR(5000, "Failed to get thread connection for rollback");
        return;
    }

    if (!db.rollback()) {
        LOG_ERROR("Failed to rollback transaction: {}", db.lastError().text().toStdString());
    }
}

std::string DbManager::getSetting(const std::string& key, const std::string& default_val) {
    if (!initialized_.load(std::memory_order_acquire)) return default_val;

    // 2026-05-19 settings cache: positive-hit only, 5s TTL, shared_mutex
    // for concurrent readers. setSetting + change-notify invalidate on
    // write so an operator PUT propagates to callers within the same
    // window even if TTL hasn't elapsed.
    const auto now = std::chrono::steady_clock::now();
    {
        std::shared_lock<std::shared_mutex> rlock(settings_cache_mu_);
        auto it = settings_cache_.find(key);
        if (it != settings_cache_.end() && now < it->second.expires_at) {
            return it->second.value;
        }
    }

    QSqlDatabase db = getThreadConnection();
    if (!db.isValid() || !db.isOpen()) return default_val;

    QSqlQuery query(db);
    query.prepare("SELECT value FROM settings WHERE key = ?");
    query.bindValue(0, QString::fromStdString(key));

    if (query.exec() && query.next()) {
        QVariant val = query.value(0);
        if (!val.isNull()) {
            std::string value = val.toString().toStdString();
            {
                std::unique_lock<std::shared_mutex> wlock(settings_cache_mu_);
                settings_cache_[key] = {value, now + kSettingsCacheTtl};
            }
            return value;
        }
    }

    // Negative result intentionally NOT cached — a key racing with a
    // pending PUT must hit DB next call rather than sticking with the
    // "not found → default" answer for the full TTL.
    return default_val;
}

bool DbManager::setSetting(const std::string& key, const std::string& value) {
    if (!initialized_.load(std::memory_order_acquire)) return false;

    QSqlDatabase db = getThreadConnection();
    if (!db.isValid() || !db.isOpen()) return false;

    QSqlQuery query(db);
    if (config_.driver == "postgresql") {
        query.prepare("INSERT INTO settings (key, value, updated_at) VALUES (?, ?, (EXTRACT(EPOCH FROM NOW()))) "
                      "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = EXCLUDED.updated_at");
    } else {
        query.prepare("INSERT OR REPLACE INTO settings (key, value, updated_at) VALUES (?, ?, (strftime('%s', 'now')))");
    }
    query.bindValue(0, QString::fromStdString(key));
    query.bindValue(1, QString::fromStdString(value));

    if (!query.exec()) {
        LOG_ERROR("Failed to insert/update setting '{}'", key);
        return false;
    }

    // 2026-05-19 settings cache write-side invalidation. Insert the
    // fresh value directly so the next reader on this thread sees it
    // without a DB round-trip; other threads with a stale entry will
    // be overwritten by this same insert (we hold the exclusive lock).
    {
        const auto now = std::chrono::steady_clock::now();
        std::unique_lock<std::shared_mutex> wlock(settings_cache_mu_);
        settings_cache_[key] = {value, now + kSettingsCacheTtl};
    }
    return true;
}

std::map<std::string, std::string> DbManager::getAllSettings() {
    std::map<std::string, std::string> result;
    if (!initialized_.load(std::memory_order_acquire)) return result;

    QSqlDatabase db = getThreadConnection();
    if (!db.isValid() || !db.isOpen()) return result;

    QSqlQuery query(db);
    if (query.exec("SELECT key, value FROM settings")) {
        while (query.next()) {
            std::string k = query.value(0).toString().toStdString();
            std::string v = query.value(1).toString().toStdString();
            result[k] = v;
        }
    }

    return result;
}

// ============================================================================
// Event Batch Writer (QW-5)
// ============================================================================

bool DbManager::enqueueEvent(const Event& event) {
    // B-3 FIX: Reject events when not accepting (shutdown or not initialized)
    if (!accepting_events_.load(std::memory_order_acquire)) {
        // 2026-05-15 DB hot-path audit: count not-accepting drops alongside
        // queue-full drops so the operator can attribute "we lost X events"
        // to either backpressure (queue-full) or shutdown race (not-accepting).
        dropped_total_.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("EventBatchWriter: not accepting events, rejecting event {}", event.id);
        return false;
    }

    // BUG-3 FIX: auto-generate UUID when caller leaves id empty.
    // Centralizes the fix for the BUG-DB-01 silent-drop pattern: any insert
    // path (ZMQ bridge, future API endpoints) is now safe even if the caller
    // forgets to set id. Building deterministic ids like "<type>-<ts>-cam<id>"
    // would collide on the second granularity → INSERT OR IGNORE drops them.
    Event ev = event;
    if (ev.id.empty()) {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<uint64_t> dis;
        uint64_t p1 = dis(gen);
        uint64_t p2 = dis(gen);
        p1 = (p1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL; // version 4
        p2 = (p2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL; // variant 10xx
        std::stringstream ss;
        ss << std::hex << std::setfill('0')
           << std::setw(8) << (p1 >> 32) << '-'
           << std::setw(4) << ((p1 >> 16) & 0xFFFF) << '-'
           << std::setw(4) << (p1 & 0xFFFF) << '-'
           << std::setw(4) << (p2 >> 48) << '-'
           << std::setw(12) << (p2 & 0xFFFFFFFFFFFFULL);
        ev.id = ss.str();
    }

    bool should_notify = false;
    std::size_t depth_after_push = 0;
    {
        std::lock_guard<std::mutex> lock(batch_queue_mutex_);
        // B-6 FIX: Cap queue at kMaxQueueSize, drop oldest with warning.
        // Drop-oldest is a deliberate choice for event telemetry: a backed-up
        // queue means the batch writer can't keep up; older events have been
        // waiting longest, are most likely to be already-stale-for-operator
        // (the user has moved on from yesterday's intrusion alert). Burst-
        // mode operators care about the present.
        if (event_queue_.size() >= kMaxQueueSize) {
            dropped_total_.fetch_add(1, std::memory_order_relaxed);
            LOG_THROTTLED_WARN(5000, "EventBatchWriter: queue full ({} events), dropping oldest event",
                     kMaxQueueSize);
            event_queue_.pop_front();
        }
        event_queue_.push_back(ev);
        depth_after_push = event_queue_.size();
        should_notify = (depth_after_push >= kBatchFlushSize);
    }
    enqueued_total_.fetch_add(1, std::memory_order_relaxed);
    // Peak-depth CAS retry, same shape as BackgroundJobRunner. Best-effort
    // max without holding the queue mutex past the push.
    {
        std::uint64_t prev_peak = peak_queue_depth_.load(std::memory_order_relaxed);
        while (depth_after_push > prev_peak &&
               !peak_queue_depth_.compare_exchange_weak(prev_peak, depth_after_push,
                                                        std::memory_order_relaxed)) {
            // retry with refreshed prev_peak
        }
    }
    if (should_notify) {
        batch_cv_.notify_one();
    }
    return true;
}

void DbManager::startBatchWriter() {
    if (batch_writer_running_.exchange(true)) return;  // Already running
    accepting_events_.store(true, std::memory_order_release);
    batch_writer_thread_ = std::thread(&DbManager::batchWriterLoop, this);
    LOG_INFO("EventBatchWriter: started (batch={}, interval={}ms, max_queue={})",
             kBatchFlushSize, kBatchFlushIntervalMs, kMaxQueueSize);
}

void DbManager::stopBatchWriter() {
    // B-3 FIX: Stop accepting events FIRST, before signalling thread to exit
    accepting_events_.store(false, std::memory_order_release);

    if (!batch_writer_running_.exchange(false)) return;  // Not running
    batch_cv_.notify_one();
    if (batch_writer_thread_.joinable()) {
        batch_writer_thread_.join();
    }
    // Final drain — flush any events still in the queue
    std::deque<Event> remaining;
    {
        std::lock_guard<std::mutex> lock(batch_queue_mutex_);
        remaining.swap(event_queue_);
    }
    if (!remaining.empty()) {
        LOG_INFO("EventBatchWriter: flushing {} remaining events on shutdown",
                 remaining.size());
        flushEventBatch(remaining);
    }
    LOG_INFO("EventBatchWriter: stopped");
}

void DbManager::batchWriterLoop() {
    LOG_INFO("EventBatchWriter: thread started");

    // Create a dedicated connection for the batch writer thread
    const QString batch_conn_name = "vms_batch_writer";
    QSqlDatabase batch_db;
    {
        std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
        if (config_.driver == "postgresql") {
            batch_db = QSqlDatabase::addDatabase("QPSQL", batch_conn_name);
            batch_db.setHostName(QString::fromStdString(config_.postgres.host));
            batch_db.setPort(config_.postgres.port);
            batch_db.setDatabaseName(QString::fromStdString(config_.postgres.database));
            batch_db.setUserName(QString::fromStdString(config_.postgres.username));
            batch_db.setPassword(QString::fromStdString(config_.postgres.password));
        } else {
            batch_db = QSqlDatabase::addDatabase("QSQLITE", batch_conn_name);
            batch_db.setDatabaseName(QString::fromStdString(config_.sqlite.path));
        }
    }

    if (!batch_db.open()) {
        LOG_ERROR("EventBatchWriter: failed to open dedicated DB connection: {}",
                  batch_db.lastError().text().toStdString());
        std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
        QSqlDatabase::removeDatabase(batch_conn_name);
        return;
    }
        // SQLite-specific optimizations for batch writer
        if (config_.driver == "sqlite") {
            QSqlQuery wal_query(batch_db);
            wal_query.exec("PRAGMA journal_mode=WAL");
            QSqlQuery timeout_query(batch_db);
            timeout_query.exec(QString("PRAGMA busy_timeout=%1").arg(config_.sqlite.busy_timeout_ms));
        }

    // Register batch writer connection for cleanup
    {
        std::lock_guard<std::mutex> lock(connection_registry_mutex_);
        registered_connections_.push_back(batch_conn_name);
    }

    // B-2 FIX (C-3): Outermost try/catch — prevents std::terminate on unhandled exception
    while (batch_writer_running_.load()) {
        try {
            std::deque<Event> batch;
            {
                std::unique_lock<std::mutex> lock(batch_queue_mutex_);
                batch_cv_.wait_for(lock,
                    std::chrono::milliseconds(kBatchFlushIntervalMs),
                    [this] {
                        return !batch_writer_running_.load()
                            || event_queue_.size() >= kBatchFlushSize;
                    });
                if (event_queue_.empty()) continue;
                batch.swap(event_queue_);
            }
            // C-2: batch_queue_mutex_ is NOT held here
            if (!flushEventBatch(batch)) {
                // B-7/C-4: flush failed — re-enqueue the batch for retry
                std::lock_guard<std::mutex> lock(batch_queue_mutex_);
                for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
                    event_queue_.push_front(std::move(*it));
                }
                LOG_WARN("EventBatchWriter: re-enqueued {} events after flush failure",
                         batch.size());
                // Back off to avoid tight retry loop
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } catch (const std::exception& e) {
            LOG_ERROR("EventBatchWriter: exception in batch loop: {}", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } catch (...) {
            LOG_ERROR("EventBatchWriter: unknown exception in batch loop");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // Close the batch writer connection
    {
        std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
        QSqlDatabase batch_db = QSqlDatabase::database(batch_conn_name, false);
        if (batch_db.isOpen()) {
            batch_db.close();
        }
        QSqlDatabase::removeDatabase(batch_conn_name);
    }

    LOG_INFO("EventBatchWriter: thread exiting");
}

bool DbManager::flushEventBatch(std::deque<Event>& batch) {
    if (batch.empty()) return true;

    const QString batch_conn_name = "vms_batch_writer";
    QSqlDatabase db;
    {
        std::lock_guard<std::mutex> conn_lock(g_db_conn_mutex);
        db = QSqlDatabase::database(batch_conn_name, false);
    }

    if (!db.isValid() || !db.isOpen()) {
        LOG_ERROR("EventBatchWriter: database connection not available, dropping {} events",
                  batch.size());
        return false;
    }

    if (!db.transaction()) {
        LOG_ERROR("EventBatchWriter: BEGIN TRANSACTION failed: {}",
                  db.lastError().text().toStdString());
        flush_failures_total_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const bool is_postgres = (config_.driver == "postgresql");

    QSqlQuery query(db);
    QString insert_sql;
    if (is_postgres) {
        insert_sql = "INSERT INTO events "
                     "(id, camera_id, event_type, description, snapshot_path, "
                     "metadata_json, timestamp, severity) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                     "ON CONFLICT (id) DO NOTHING";
    } else {
        insert_sql = "INSERT OR IGNORE INTO events "
                     "(id, camera_id, event_type, description, snapshot_path, "
                     "metadata_json, timestamp, severity) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    }

    if (!query.prepare(insert_sql)) {
        LOG_ERROR("EventBatchWriter: prepare failed: {}", query.lastError().text().toStdString());
        db.rollback();
        flush_failures_total_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    int inserted = 0;
    std::uint64_t row_failures_this_batch = 0;
    for (const auto& event : batch) {
        // 2026-05-15 DB hot-path audit: SAVEPOINT around each insert on
        // Postgres so one poisoned row (NOT NULL violation / type mismatch
        // / CHECK violation) doesn't abort the whole transaction. Without
        // this, Postgres MVCC marks the tx as aborted on the first error
        // and every subsequent INSERT fails with "current transaction is
        // aborted, commands ignored until end". Commit then fails →
        // re-enqueue the WHOLE batch → infinite retry of the poison.
        // SQLite already tolerates per-statement errors mid-transaction.
        QSqlQuery savepoint(db);
        if (is_postgres) {
            if (!savepoint.exec("SAVEPOINT vms_row_sp")) {
                LOG_THROTTLED_WARN(5000, "EventBatchWriter: SAVEPOINT failed: {}",
                                   savepoint.lastError().text().toStdString());
            }
        }

        query.bindValue(0, QString::fromStdString(event.id));
        query.bindValue(1, event.camera_id);
        query.bindValue(2, QString::fromStdString(event.event_type));
        query.bindValue(3, QString::fromStdString(event.description));
        query.bindValue(4, QString::fromStdString(event.snapshot_path));
        query.bindValue(5, QString::fromStdString(event.metadata_json));
        query.bindValue(6, static_cast<qint64>(event.timestamp));
        query.bindValue(7, QString::fromStdString(event.severity));

        if (query.exec()) {
            ++inserted;
            if (is_postgres) {
                // Release savepoint to drop the per-row marker; keeps tx
                // savepoint stack bounded across the batch.
                savepoint.exec("RELEASE SAVEPOINT vms_row_sp");
            }
        } else {
            ++row_failures_this_batch;
            LOG_WARN("EventBatchWriter: insert failed for event {}: {}",
                     event.id, query.lastError().text().toStdString());
            if (is_postgres) {
                // Roll back the per-row savepoint so the outer transaction
                // remains usable. The poisoned event is dropped (will not
                // retry); this is intentional — re-enqueueing a poison
                // row is precisely the infinite-retry loop we're avoiding.
                if (!savepoint.exec("ROLLBACK TO SAVEPOINT vms_row_sp")) {
                    LOG_ERROR("EventBatchWriter: ROLLBACK TO SAVEPOINT failed: {}",
                              savepoint.lastError().text().toStdString());
                }
                savepoint.exec("RELEASE SAVEPOINT vms_row_sp");
            }
        }
    }
    if (row_failures_this_batch > 0) {
        row_failures_total_.fetch_add(row_failures_this_batch, std::memory_order_relaxed);
    }

    // B-7 FIX: On COMMIT failure, ROLLBACK and return false so caller re-enqueues
    if (!db.commit()) {
        LOG_ERROR("EventBatchWriter: COMMIT failed: {} — rolling back",
                  db.lastError().text().toStdString());
        db.rollback();
        flush_failures_total_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (inserted > 0) {
        flushed_total_.fetch_add(static_cast<std::uint64_t>(inserted), std::memory_order_relaxed);
        LOG_DEBUG("EventBatchWriter: flushed {}/{} events",
                  inserted, static_cast<int>(batch.size()));
    }
    return true;
}

DbManager::BatchWriterStats DbManager::batchWriterStats() const {
    BatchWriterStats s;
    s.max_queue_size           = kMaxQueueSize;
    s.batch_flush_size         = kBatchFlushSize;
    s.enqueued_total           = enqueued_total_.load(std::memory_order_relaxed);
    s.dropped_total            = dropped_total_.load(std::memory_order_relaxed);
    s.flushed_total            = flushed_total_.load(std::memory_order_relaxed);
    s.flush_failures_total     = flush_failures_total_.load(std::memory_order_relaxed);
    s.row_failures_total       = row_failures_total_.load(std::memory_order_relaxed);
    s.peak_queue_depth         = peak_queue_depth_.load(std::memory_order_relaxed);
    s.running                  = batch_writer_running_.load(std::memory_order_relaxed);
    s.accepting                = accepting_events_.load(std::memory_order_relaxed);
    // Try-lock for queue depth so stats never contends the producer hot path.
    // If contended we leave current_queue_depth=0; the next poll typically
    // succeeds. Operators rely on enqueued_total - flushed_total for the real
    // backlog estimate, not this point-in-time number.
    std::unique_lock<std::mutex> lk(batch_queue_mutex_, std::try_to_lock);
    if (lk.owns_lock()) {
        s.current_queue_depth = event_queue_.size();
    }
    return s;
}

// ── WAL Checkpoint Thread (SQLite only) ─────────────────────────────────────
// 2026-06-01 BUG-DB-WAL-UNBOUNDED-GROWTH-01.
//
// SQLite's built-in wal_autocheckpoint runs PASSIVE every ~1000 pages (~4MB
// WAL growth). PASSIVE never shrinks the .wal file — under sustained reader
// load (analytics, dashboard polls) the WAL keeps growing because PASSIVE
// can't reclaim space while readers hold a snapshot. TRUNCATE is the only
// mode that returns space to the filesystem.
//
// This thread runs TRUNCATE every wal_checkpoint_seconds. On a busy DB
// TRUNCATE may return busy=1 (no work done, retry next tick); on a quieter
// tick it succeeds and the .wal file shrinks. Worst-case behaviour under
// 100x load: the file grows during sustained writes, shrinks during quiet
// windows. Operator can lower the interval to checkpoint more aggressively.
//
// busy_timeout (set on this thread's own connection) bounds how long TRUNCATE
// waits for readers before giving up — already 5s by default. Dedicated
// thread; never blocks the producer hot path.
void DbManager::startWalCheckpoint() {
    if (!shouldRunWalCheckpoint(config_.driver, config_.sqlite.wal_checkpoint_seconds)) {
        LOG_INFO("WalCheckpoint: disabled (driver={}, seconds={})",
                 config_.driver, config_.sqlite.wal_checkpoint_seconds);
        return;
    }
    if (wal_checkpoint_running_.exchange(true)) return;  // Already running
    wal_checkpoint_thread_ = std::thread(&DbManager::walCheckpointLoop, this);
    LOG_INFO("WalCheckpoint: started (interval={}s)",
             config_.sqlite.wal_checkpoint_seconds);
}

void DbManager::stopWalCheckpoint() {
    if (!wal_checkpoint_running_.exchange(false)) return;  // Not running
    wal_checkpoint_cv_.notify_all();
    if (wal_checkpoint_thread_.joinable()) {
        wal_checkpoint_thread_.join();
    }
    LOG_INFO("WalCheckpoint: stopped (truncated={}, busy={}, failed={})",
             wal_checkpoint_total_.load(std::memory_order_relaxed),
             wal_checkpoint_busy_.load(std::memory_order_relaxed),
             wal_checkpoint_failures_.load(std::memory_order_relaxed));
}

void DbManager::walCheckpointLoop() {
    const auto interval = std::chrono::seconds(
        config_.sqlite.wal_checkpoint_seconds);
    LOG_INFO("WalCheckpoint: thread started (interval={}s)", interval.count());

    while (wal_checkpoint_running_.load(std::memory_order_acquire)) {
        // Wait for either the interval to elapse or shutdown notification.
        // CV lock is released BEFORE the PRAGMA so it cannot serialise the
        // shutdown signal behind a long-running TRUNCATE.
        {
            std::unique_lock<std::mutex> lk(wal_checkpoint_mu_);
            wal_checkpoint_cv_.wait_for(lk, interval, [this] {
                return !wal_checkpoint_running_.load(std::memory_order_acquire);
            });
        }
        if (!wal_checkpoint_running_.load(std::memory_order_acquire)) break;

        try {
            auto db = getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                LOG_THROTTLED_WARN(5000,
                    "WalCheckpoint: no DB connection — skip tick");
                wal_checkpoint_failures_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            QSqlQuery q(db);
            // wal_checkpoint(TRUNCATE) result columns:
            //   0: busy flag (1 = readers held the WAL lock; no work done)
            //   1: total pages currently in WAL
            //   2: pages successfully checkpointed
            if (!q.exec("PRAGMA wal_checkpoint(TRUNCATE)")) {
                LOG_THROTTLED_WARN(5000,
                    "WalCheckpoint: PRAGMA exec failed: {}",
                    q.lastError().text().toStdString());
                wal_checkpoint_failures_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (q.next()) {
                const int busy  = q.value(0).toInt();
                const int total = q.value(1).toInt();
                const int done  = q.value(2).toInt();
                if (busy != 0) {
                    wal_checkpoint_busy_.fetch_add(1, std::memory_order_relaxed);
                    LOG_THROTTLED_INFO(60000,
                        "WalCheckpoint: busy — readers hold WAL lock "
                        "(total_pages={}, checkpointed={})",
                        total, done);
                } else {
                    wal_checkpoint_total_.fetch_add(1, std::memory_order_relaxed);
                    if (total > 0) {
                        LOG_THROTTLED_INFO(60000,
                            "WalCheckpoint: truncated {} of {} pages",
                            done, total);
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG_THROTTLED_WARN(5000, "WalCheckpoint: exception: {}", e.what());
            wal_checkpoint_failures_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            LOG_THROTTLED_WARN(5000, "WalCheckpoint: unknown exception");
            wal_checkpoint_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    LOG_INFO("WalCheckpoint: thread exiting");
}

} // namespace database
} // namespace vms
