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
    };
    
    // AI Server configuration
    struct AIServerConfig {
        bool enabled = true;
        int base_zmq_port = 5555;
        int zmq_timeout_ms = 1000;
        int reconnect_interval_sec = 5;
    };

    // Batch inference configuration (M-1)
    struct BatchInferenceConfig {
        bool enabled = false;
        std::string engine_path = "models\\yolo11m.engine";
        int max_batch_size = 4;
        int max_wait_ms = 5;
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
    BatchInferenceConfig batch_inference_;
    LoggingConfig logging_;
    StorageConfig storage_;
    
    // YAML root node
    YAML::Node config_;
};

} // namespace vms
