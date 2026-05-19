#pragma once

#include <string>
#include <memory>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace vms {

/**
 * @brief Configuration manager for VMS Backend
 * 
 * Loads configuration from YAML file and provides access to all settings.
 * Singleton pattern to ensure single configuration instance.
 */
class Config {
public:
    // Server configuration
    struct ServerConfig {
        std::string host = "0.0.0.0";
        int port = 8000;
        int threads = 4;
        std::string log_level = "info";
    };
    
    // Database configuration
    struct DatabaseConfig {
        std::string driver = "sqlite"; // "postgresql" or "sqlite"
        
        // PostgreSQL settings
        struct PostgresConfig {
            std::string host = "localhost";
            int port = 5432;
            std::string database = "vms_ai";
            std::string username = "postgres";
            std::string password = "";
            int pool_size = 10;
        } postgres;

        // SQLite settings
        struct SqliteConfig {
            std::string path = "./data/events.db";
            int busy_timeout_ms = 5000;
        } sqlite;
        
        // Legacy support/alias
        std::string path = "./data/events.db";
    };

    // Storage configuration (MinIO)
    struct StorageConfig {
        std::string driver = "local"; // "minio" or "local"
        struct MinioConfig {
            std::string endpoint = "http://localhost:9000";
            std::string access_key = "minioadmin";
            std::string secret_key = "minioadmin";
            std::string bucket_recordings = "vms-recordings";
            std::string bucket_snapshots = "vms-snapshots";
        } minio;
    };
    
    // Authentication configuration
    struct AuthConfig {
        bool enabled = true;
        std::string secret_key = "vms-ai-secret-key-change-me-in-production";
        std::string algorithm = "HS256";
        int token_expire_minutes = 10080;
    };

    // Presigned media URL configuration
    struct MediaSigningConfig {
        // If true, presigned URLs must include signed scope fields.
        bool strict_scope_required = false;
        // C6: Maximum TTL enforced on sign and verify. No presigned URL can exceed this.
        int max_ttl_seconds = 900;           // 15 minutes — production ceiling
        int default_ttl_seconds = 120;
        int snapshot_ttl_seconds = 180;
        int event_video_ttl_seconds = 300;
        int recording_video_ttl_seconds = 300;
        int segment_video_ttl_seconds = 300;
        int camera_frame_ttl_seconds = 60;
        int storage_ttl_seconds = 180;
        // M8: Allow legacy unscoped presign format (path|exp). Disable after migration.
        bool allow_legacy_unsigned = false;
    };

    // LDAP configuration
    struct LdapConfig {
        bool isEnabled = false;
        std::string ldapHost = "";
        int ldapPort = 389;
        std::string ldapDomain = "";
    };
    
    // CORS configuration
    struct CorsConfig {
        bool enabled = true;
        std::vector<std::string> origins = {"*"};
        std::vector<std::string> methods = {"GET", "POST", "PUT", "DELETE", "OPTIONS"};
        std::vector<std::string> headers = {"*"};
        bool credentials = true;
    };
    
    // WebSocket configuration
    struct WebSocketConfig {
        int port = 8083;
        int heartbeat_interval_sec = 30;
        int ping_interval_sec = 20;
        int ping_timeout_sec = 10;
        int max_message_size_mb = 10;
        int update_interval_ms = 100;
        // 2026-05-17 WS observability trifecta: caps + applied msg size cap.
        // Both caps default to "off" (0) so existing deployments aren't
        // suddenly rejecting connections after a binary upgrade — operators
        // opt in by setting non-zero values in backend.yaml. Reasonable
        // starting points: global=200, per_ip=10 for a typical control-room
        // deployment; raise both for high-density videowall mosaics.
        int max_connections_global = 0;   // 0 = unlimited
        int max_connections_per_ip = 0;   // 0 = unlimited
    };
    
    // AI Server configuration
    struct AIServerConfig {
        bool enabled = true;
        int base_zmq_port = 5555;
        int zmq_timeout_ms = 1000;
        int reconnect_interval_sec = 5;
    };

    // 2026-05-19 AI detection tuning thresholds. Exported into the parent
    // process env BEFORE ai_worker_v2 spawn (camera_pipeline_manager uses
    // QProcess which inherits env). Operators can also set env vars
    // directly — env takes precedence so emergency override doesn't need
    // a config edit + restart cycle. Values land in ai_worker_v2's
    // post-detection filter (src/ai_worker/main.cpp).
    //
    // min_person_height_px: drop person-class detections with bbox height
    // smaller than this (px in the source frame). Combats distant-noise
    // false positives (vegetation, fence texture, shadow). Default 20 is
    // a permissive baseline; the 2026-05-08 Fix-B operator
    // ("thùng rác cũng ra người") chose 40 for their specific deployment.
    // If your camera mount sees people at <20 px height your scene is
    // either very wide-angle or the camera is too far for face
    // recognition to be useful anyway.
    struct AIDetectionConfig {
        float min_person_height_px = 20.0f;
    };

    // Batch inference configuration (M-1)
    struct BatchInferenceConfig {
        bool enabled = false;
        std::string engine_path = "models\\yolo11m.engine";
        int max_batch_size = 4;
        int max_wait_ms = 5;
    };
    
    // 2026-05-15 Tier 1 RBAC sweep: trusted proxies for X-Forwarded-For.
    //
    // Pre-fix `req.get_header_value("X-Forwarded-For")` was honoured
    // unconditionally → attacker with direct backend access could spoof
    // client IP for /api/login rate-limiter bypass and /api/ws/ticket
    // IP-binding poisoning. With `trusted_proxies` configured, XFF is
    // honoured ONLY when the immediate peer (req.remote_ip_address) is in
    // the list — i.e. the request came through a trusted reverse proxy.
    //
    // Default empty list → XFF always ignored, peer IP always wins. This
    // is the safe default for deployments WITHOUT a reverse proxy. When
    // operators put nginx/HAProxy/caddy in front, they list its IPs here
    // (e.g. ["127.0.0.1", "::1"] for sidecar; "10.0.0.5" for separate-host).
    //
    // CIDR not yet supported — operators list each proxy IP explicitly.
    // "loopback" is a convenience alias expanded to 127.0.0.1 + ::1.
    struct SecurityConfig {
        std::vector<std::string> trusted_proxies;
    };

    // 2026-05-19 BUG-ALERT-CASCADE-POOL-01 phase 2: operator-tunable pool
    // sizing for the 4 alert delivery channels. Defaults match the
    // hardcoded values shipped 2026-05-17: webhook 4/256, sms 2/128,
    // telegram 2/128, alarm 1/64. Sizes are anchored to vendor rate
    // limits (Twilio ~1 msg/s/number → 2 SMS workers; Telegram 30 msg/s
    // /chat → 2 telegram workers; webhook + alarm sized for burst absorb).
    // Operators tune via `alert_delivery.{webhook,sms,telegram,alarm}.
    // {workers, queue_size}` in backend.yaml.
    //
    // Reading order: each pool's BackgroundJobRunner is constructed via
    // a function-local static at first use of the *Runner() accessor in
    // alert_delivery.cpp. Config must be loaded before any alert delivery
    // path runs — main.cpp does `Config::loadFromFile` well before AI
    // event ingestion starts, so the read order is safe by-construction.
    // After the first runner-accessor call the values are captured for
    // the program's lifetime; live config reload is NOT supported here.
    struct AlertDeliveryConfig {
        struct PoolConfig {
            int workers = 0;     // 0/<=0 → use channel default
            int queue_size = 0;  // 0/<=0 → use channel default
        };
        PoolConfig webhook;
        PoolConfig sms;
        PoolConfig telegram;
        PoolConfig alarm;
    };

    // Logging configuration
    struct LoggingConfig {
        std::string level = "info";
        std::string file_path = "./logs/cpp_backend.log";
        bool console_output = true;
        bool file_output = true;
        int max_file_size_mb = 100;
        int max_files = 5;
        std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v";
    };

public:
    /**
     * @brief Get singleton instance
     */
    static Config& getInstance();
    
    /**
     * @brief Load configuration from YAML file
     * @param filepath Path to YAML configuration file
     * @return true if successful, false otherwise
     */
    bool loadFromFile(const std::string& filepath);
    
    /**
     * @brief Get server configuration
     */
    const ServerConfig& getServerConfig() const { return server_; }
    
    /**
     * @brief Get database configuration
     */
    const DatabaseConfig& getDatabaseConfig() const { return database_; }
    
    /**
     * @brief Get auth configuration
     */
    const AuthConfig& getAuthConfig() const { return auth_; }
    const MediaSigningConfig& getMediaSigningConfig() const { return media_signing_; }

    /**
     * @brief Get LDAP configuration
     */
    const LdapConfig& getLdapConfig() const { return ldap_; }
    
    /**
     * @brief Get CORS configuration
     */
    const CorsConfig& getCorsConfig() const { return cors_; }
    
    /**
     * @brief Get WebSocket configuration
     */
    const WebSocketConfig& getWebSocketConfig() const { return websocket_; }
    
    /**
     * @brief Get AI server configuration
     */
    const AIServerConfig& getAIServerConfig() const { return ai_server_; }

    /**
     * @brief Get AI detection tuning thresholds (min_person_height_px etc.)
     */
    const AIDetectionConfig& getAIDetectionConfig() const { return ai_detection_; }

    /**
     * @brief Get batch inference configuration
     */
    const BatchInferenceConfig& getBatchInferenceConfig() const { return batch_inference_; }
    
    /**
     * @brief Get logging configuration
     */
    const LoggingConfig& getLoggingConfig() const { return logging_; }

    /**
     * @brief Get storage configuration
     */
    const StorageConfig& getStorageConfig() const { return storage_; }

    /**
     * @brief Get security configuration (trusted proxies, etc.)
     */
    const SecurityConfig& getSecurityConfig() const { return security_; }

    /**
     * @brief Get alert delivery pool sizing (0 fields mean "use default").
     */
    const AlertDeliveryConfig& getAlertDeliveryConfig() const { return alert_delivery_; }
    
    /**
     * @brief Print configuration to console
     */
    void print() const;

private:
    Config() = default;
    ~Config() = default;
    
    // Delete copy and move constructors/operators
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&) = delete;
    Config& operator=(Config&&) = delete;
    
    // Configuration sections
    ServerConfig server_;
    DatabaseConfig database_;
    AuthConfig auth_;
    MediaSigningConfig media_signing_;
    LdapConfig ldap_;
    CorsConfig cors_;
    WebSocketConfig websocket_;
    AIServerConfig ai_server_;
    AIDetectionConfig ai_detection_;
    BatchInferenceConfig batch_inference_;
    LoggingConfig logging_;
    StorageConfig storage_;
    SecurityConfig security_;
    AlertDeliveryConfig alert_delivery_;
    
    // YAML root node
    YAML::Node config_;
};

} // namespace vms
