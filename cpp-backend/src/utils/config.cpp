#include "utils/config.h"
#include "utils/logger.h"
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <exception>

namespace vms {

static bool iequals(std::string a, std::string b) {
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return a == b;
}

static std::string getEnvStr(const char* key) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string();
}

static bool tryGetEnvInt(const char* key, int& out_value) {
    const auto raw = getEnvStr(key);
    if (raw.empty()) {
        return false;
    }

    try {
        size_t parsed_chars = 0;
        const int parsed_value = std::stoi(raw, &parsed_chars);
        if (parsed_chars != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        out_value = parsed_value;
        return true;
    } catch (const std::exception&) {
        LOG_WARN("Ignoring invalid integer environment variable {}={}", key, raw);
        return false;
    }
}

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

bool Config::loadFromFile(const std::string& filepath) {
    try {
        // Load YAML file
        config_ = YAML::LoadFile(filepath);
        
        // Parse server configuration
        if (config_["server"]) {
            auto server = config_["server"];
            server_.host = server["host"].as<std::string>(server_.host);
            server_.port = server["port"].as<int>(server_.port);
            server_.threads = server["threads"].as<int>(server_.threads);
            server_.log_level = server["log_level"].as<std::string>(server_.log_level);
        }
        
        // Parse database configuration
        if (config_["database"]) {
            auto db = config_["database"];
            database_.driver = db["driver"].as<std::string>(database_.driver);
            
            if (db["postgresql"]) {
                auto pg = db["postgresql"];
                database_.postgres.host = pg["host"].as<std::string>(database_.postgres.host);
                database_.postgres.port = pg["port"].as<int>(database_.postgres.port);
                database_.postgres.database = pg["database"].as<std::string>(database_.postgres.database);
                database_.postgres.username = pg["username"].as<std::string>(database_.postgres.username);
                database_.postgres.password = pg["password"].as<std::string>(database_.postgres.password);
                database_.postgres.pool_size = pg["pool_size"].as<int>(database_.postgres.pool_size);
            }

            if (db["sqlite"]) {
                auto sq = db["sqlite"];
                database_.sqlite.path = sq["path"].as<std::string>(database_.sqlite.path);
                database_.sqlite.busy_timeout_ms = sq["busy_timeout_ms"].as<int>(database_.sqlite.busy_timeout_ms);
            }
            
            // Legacy path support
            if (db["path"]) {
                database_.path = db["path"].as<std::string>(database_.path);
                database_.sqlite.path = database_.path;
            }
        }

        // Parse storage configuration
        if (config_["storage"]) {
            auto storage = config_["storage"];
            storage_.driver = storage["driver"].as<std::string>(storage_.driver);
            storage_.required = storage["required"].as<bool>(storage_.required);
            if (storage["minio"]) {
                auto minio = storage["minio"];
                storage_.minio.endpoint = minio["endpoint"].as<std::string>(storage_.minio.endpoint);
                storage_.minio.access_key = minio["access_key"].as<std::string>(storage_.minio.access_key);
                storage_.minio.secret_key = minio["secret_key"].as<std::string>(storage_.minio.secret_key);
                storage_.minio.bucket_recordings = minio["bucket_recordings"].as<std::string>(storage_.minio.bucket_recordings);
                storage_.minio.bucket_snapshots = minio["bucket_snapshots"].as<std::string>(storage_.minio.bucket_snapshots);
            }
        }

        // SEC: Allow DB/storage secrets to be injected via environment variables
        // (keeps YAML safe to commit)
        {
            const auto pg_password = getEnvStr("VMS_PG_PASSWORD");
            if (!pg_password.empty()) {
                database_.postgres.password = pg_password;
                LOG_INFO("Loaded PostgreSQL password from VMS_PG_PASSWORD environment variable");
            }
            const auto minio_access = getEnvStr("VMS_MINIO_ACCESS_KEY");
            if (!minio_access.empty()) {
                storage_.minio.access_key = minio_access;
                LOG_INFO("Loaded MinIO access key from VMS_MINIO_ACCESS_KEY environment variable");
            }
            const auto minio_secret = getEnvStr("VMS_MINIO_SECRET_KEY");
            if (!minio_secret.empty()) {
                storage_.minio.secret_key = minio_secret;
                LOG_INFO("Loaded MinIO secret key from VMS_MINIO_SECRET_KEY environment variable");
            }
            // H7: env override for storage.required (= "1" turns it on,
            // "0" / "false" / "no" / unset leaves whatever the YAML says).
            const auto storage_required_env = getEnvStr("VMS_STORAGE_REQUIRED");
            if (!storage_required_env.empty()) {
                const bool on = (storage_required_env == "1" ||
                                 storage_required_env == "true" ||
                                 storage_required_env == "TRUE" ||
                                 storage_required_env == "yes");
                storage_.required = on;
                LOG_INFO("Storage 'required' flag set from VMS_STORAGE_REQUIRED env var: {}", on);
            }
        }

        // Parse authentication configuration
        if (config_["auth"]) {
            auto auth = config_["auth"];
            auth_.enabled = auth["enabled"].as<bool>(auth_.enabled);
            // SEC-002: Read JWT secret from environment variable first
            const auto env_secret = getEnvStr("VMS_JWT_SECRET");
            if (!env_secret.empty()) {
                auth_.secret_key = env_secret;
                LOG_INFO("Loaded JWT secret from VMS_JWT_SECRET environment variable");
            } else {
                auth_.secret_key = auth["secret_key"].as<std::string>(auth_.secret_key);
            }
            auth_.algorithm = auth["algorithm"].as<std::string>(auth_.algorithm);
            auth_.token_expire_minutes = auth["token_expire_minutes"].as<int>(auth_.token_expire_minutes);
            
            // Parse LDAP configuration
            if (auth["ldap"]) {
                auto ldap = auth["ldap"];
                ldap_.isEnabled = ldap["enabled"].as<bool>(ldap_.isEnabled);
                ldap_.ldapHost = ldap["host"].as<std::string>(ldap_.ldapHost);
                ldap_.ldapPort = ldap["port"].as<int>(ldap_.ldapPort);
                ldap_.ldapDomain = ldap["domain"].as<std::string>(ldap_.ldapDomain);
            }
        }

        // Parse presigned media URL configuration
        if (config_["media_signing"]) {
            auto media = config_["media_signing"];
            media_signing_.strict_scope_required =
                media["strict_scope_required"].as<bool>(media_signing_.strict_scope_required);
            media_signing_.max_ttl_seconds =
                media["max_ttl_seconds"].as<int>(media_signing_.max_ttl_seconds);
            media_signing_.allow_legacy_unsigned =
                media["allow_legacy_unsigned"].as<bool>(media_signing_.allow_legacy_unsigned);
            media_signing_.default_ttl_seconds =
                media["default_ttl_seconds"].as<int>(media_signing_.default_ttl_seconds);
            media_signing_.snapshot_ttl_seconds =
                media["snapshot_ttl_seconds"].as<int>(media_signing_.snapshot_ttl_seconds);
            media_signing_.event_video_ttl_seconds =
                media["event_video_ttl_seconds"].as<int>(media_signing_.event_video_ttl_seconds);
            media_signing_.recording_video_ttl_seconds =
                media["recording_video_ttl_seconds"].as<int>(media_signing_.recording_video_ttl_seconds);
            media_signing_.segment_video_ttl_seconds =
                media["segment_video_ttl_seconds"].as<int>(media_signing_.segment_video_ttl_seconds);
            media_signing_.camera_frame_ttl_seconds =
                media["camera_frame_ttl_seconds"].as<int>(media_signing_.camera_frame_ttl_seconds);
            media_signing_.storage_ttl_seconds =
                media["storage_ttl_seconds"].as<int>(media_signing_.storage_ttl_seconds);
        }

        // SEC-C1: Refuse weak or default JWT secret unconditionally when auth is enabled.
        // The old is_dev bypass is removed — omitting VMS_ENV is not a safe default in production.
        if (auth_.enabled) {
            const std::string default_secret = "vms-ai-secret-key-change-me-in-production";
            const auto allow_default = getEnvStr("VMS_ALLOW_DEFAULT_SECRET");
            if (auth_.secret_key == default_secret) {
                if (allow_default == "1") {
                    LOG_CRITICAL("SEC-C1 BYPASS ACTIVE: running with default JWT secret. "
                                 "Set VMS_JWT_SECRET to a random secret of at least 32 characters.");
                } else {
                    LOG_ERROR("Refusing to start: default JWT secret detected. "
                              "Set VMS_JWT_SECRET env var (min 32 chars) or set VMS_ALLOW_DEFAULT_SECRET=1 "
                              "only for local development.");
                    return false;
                }
            }
            if (auth_.secret_key.size() < 32) {
                LOG_ERROR("Refusing to start: JWT secret is too short ({} chars). Minimum is 32 characters.",
                          auth_.secret_key.size());
                return false;
            }
        }
        
        // Parse CORS configuration
        if (config_["cors"]) {
            auto cors = config_["cors"];
            cors_.enabled = cors["enabled"].as<bool>(cors_.enabled);
            
            if (cors["origins"]) {
                cors_.origins.clear();
                for (const auto& origin : cors["origins"]) {
                    cors_.origins.push_back(origin.as<std::string>());
                }
            }
            
            if (cors["methods"]) {
                cors_.methods.clear();
                for (const auto& method : cors["methods"]) {
                    cors_.methods.push_back(method.as<std::string>());
                }
            }
            
            if (cors["headers"]) {
                cors_.headers.clear();
                for (const auto& header : cors["headers"]) {
                    cors_.headers.push_back(header.as<std::string>());
                }
            }
            
            cors_.credentials = cors["credentials"].as<bool>(cors_.credentials);
        }
        
        // Parse WebSocket configuration
        if (config_["websocket"]) {
            auto ws = config_["websocket"];
            websocket_.port = ws["port"].as<int>(websocket_.port);
            websocket_.heartbeat_interval_sec = ws["heartbeat_interval_sec"].as<int>(websocket_.heartbeat_interval_sec);
            websocket_.ping_interval_sec = ws["ping_interval_sec"].as<int>(websocket_.ping_interval_sec);
            websocket_.ping_timeout_sec = ws["ping_timeout_sec"].as<int>(websocket_.ping_timeout_sec);
            websocket_.max_message_size_mb = ws["max_message_size_mb"].as<int>(websocket_.max_message_size_mb);
            websocket_.update_interval_ms = ws["update_interval_ms"].as<int>(websocket_.update_interval_ms);
            websocket_.max_connections_global = ws["max_connections_global"].as<int>(websocket_.max_connections_global);
            websocket_.max_connections_per_ip = ws["max_connections_per_ip"].as<int>(websocket_.max_connections_per_ip);
        }

        // 2026-05-19 BUG-ALERT-CASCADE-POOL-01 phase 2: per-channel pool
        // sizing. Missing keys / non-positive values leave the field at
        // its 0 sentinel; alert_delivery.cpp factories fall back to the
        // channel's hardcoded default in that case.
        if (config_["alert_delivery"]) {
            auto ad = config_["alert_delivery"];
            auto load = [](YAML::Node n, AlertDeliveryConfig::PoolConfig& pc) {
                if (!n) return;
                if (n["workers"])    pc.workers    = n["workers"].as<int>(pc.workers);
                if (n["queue_size"]) pc.queue_size = n["queue_size"].as<int>(pc.queue_size);
            };
            load(ad["webhook"],  alert_delivery_.webhook);
            load(ad["sms"],      alert_delivery_.sms);
            load(ad["telegram"], alert_delivery_.telegram);
            load(ad["alarm"],    alert_delivery_.alarm);
        }

        if (tryGetEnvInt("PORT", server_.port)) {
            LOG_INFO("Loaded server port from PORT environment variable: {}", server_.port);
        }
        if (tryGetEnvInt("WS_PORT", websocket_.port)) {
            LOG_INFO("Loaded WebSocket port from WS_PORT environment variable: {}", websocket_.port);
        }
        
        // Parse AI Server configuration
        if (config_["ai_server"]) {
            auto ai = config_["ai_server"];
            ai_server_.enabled = ai["enabled"].as<bool>(ai_server_.enabled);
            ai_server_.base_zmq_port = ai["base_zmq_port"].as<int>(ai_server_.base_zmq_port);
            ai_server_.zmq_timeout_ms = ai["zmq_timeout_ms"].as<int>(ai_server_.zmq_timeout_ms);
            ai_server_.reconnect_interval_sec = ai["reconnect_interval_sec"].as<int>(ai_server_.reconnect_interval_sec);
        }

        // 2026-05-19 AI detection tuning thresholds — see config.h comment
        // for trade-offs. Exported into the parent process env in main.cpp
        // so ai_worker_v2 children inherit via QProcess.
        if (config_["ai"]) {
            auto ai = config_["ai"];
            ai_detection_.min_person_height_px =
                ai["min_person_height_px"].as<float>(ai_detection_.min_person_height_px);

            // 2026-05-20 PPE engine class-label config. Each field is
            // optional — absent fields keep the struct defaults
            // (PPE-YOLOv8m 6-class engine shipped 2026-05-19). Sanity:
            // `labels` empty → fall back to defaults rather than disable
            // labelling entirely.
            if (ai["ppe"]) {
                auto ppe = ai["ppe"];
                if (ppe["labels"] && ppe["labels"].IsSequence()) {
                    std::vector<std::string> parsed;
                    parsed.reserve(ppe["labels"].size());
                    for (const auto& node : ppe["labels"]) {
                        parsed.push_back(node.as<std::string>(""));
                    }
                    if (!parsed.empty()) ai_detection_.ppe.labels = std::move(parsed);
                }
                ai_detection_.ppe.person_class =
                    ppe["person_class"].as<int>(ai_detection_.ppe.person_class);
                ai_detection_.ppe.helmet_class =
                    ppe["helmet_class"].as<int>(ai_detection_.ppe.helmet_class);
                ai_detection_.ppe.vest_class =
                    ppe["vest_class"].as<int>(ai_detection_.ppe.vest_class);
                ai_detection_.ppe.gloves_class =
                    ppe["gloves_class"].as<int>(ai_detection_.ppe.gloves_class);
                ai_detection_.ppe.mask_class =
                    ppe["mask_class"].as<int>(ai_detection_.ppe.mask_class);
                ai_detection_.ppe.boots_class =
                    ppe["boots_class"].as<int>(ai_detection_.ppe.boots_class);
            }
        }
        
        // Parse batch inference configuration
        if (config_["batch_inference"]) {
            auto bi = config_["batch_inference"];
            batch_inference_.enabled = bi["enabled"].as<bool>(batch_inference_.enabled);
            batch_inference_.engine_path = bi["engine_path"].as<std::string>(batch_inference_.engine_path);
            batch_inference_.max_batch_size = bi["max_batch_size"].as<int>(batch_inference_.max_batch_size);
            batch_inference_.max_wait_ms = bi["max_wait_ms"].as<int>(batch_inference_.max_wait_ms);
        }

        // Parse security configuration (2026-05-15 Tier 1: trusted_proxies).
        // Empty list = XFF always ignored (safe default). Expand "loopback"
        // alias to both IPv4 + IPv6 loopback so operators on Windows sidecar
        // proxies don't have to remember "::1".
        if (config_["security"]) {
            auto sec = config_["security"];
            if (sec["trusted_proxies"]) {
                security_.trusted_proxies.clear();
                for (const auto& p : sec["trusted_proxies"]) {
                    std::string ip = p.as<std::string>();
                    if (ip == "loopback") {
                        security_.trusted_proxies.push_back("127.0.0.1");
                        security_.trusted_proxies.push_back("::1");
                    } else if (!ip.empty()) {
                        security_.trusted_proxies.push_back(std::move(ip));
                    }
                }
            }
        }

        // Parse logging configuration
        if (config_["logging"]) {
            auto log = config_["logging"];
            logging_.level = log["level"].as<std::string>(logging_.level);
            logging_.file_path = log["file_path"].as<std::string>(logging_.file_path);
            logging_.console_output = log["console_output"].as<bool>(logging_.console_output);
            logging_.file_output = log["file_output"].as<bool>(logging_.file_output);
            logging_.max_file_size_mb = log["max_file_size_mb"].as<int>(logging_.max_file_size_mb);
            logging_.max_files = log["max_files"].as<int>(logging_.max_files);
            logging_.pattern = log["pattern"].as<std::string>(logging_.pattern);
        }
        
        return true;
        
    } catch (const YAML::Exception& e) {
        LOG_ERROR("YAML parsing error: {}", e.what());
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Error loading config: {}", e.what());
        return false;
    }
}

void Config::print() const {
    LOG_INFO("Configuration:");
    LOG_INFO("  Server: host={}, port={}, threads={}, log_level={}",
             server_.host, server_.port, server_.threads, server_.log_level);
    LOG_INFO("  Database: driver={}, host={}, db={}",
             database_.driver, database_.postgres.host, database_.postgres.database);
    LOG_INFO("  Storage: driver={}, endpoint={}",
             storage_.driver, storage_.minio.endpoint);
    LOG_INFO("  Auth: enabled={}, algorithm={}, LDAP={}",
             auth_.enabled, auth_.algorithm, ldap_.isEnabled);
    LOG_INFO("  Media Signing: strict_scope_required={}, default_ttl={}s",
             media_signing_.strict_scope_required, media_signing_.default_ttl_seconds);
    LOG_INFO("  WebSocket: update_interval={}ms", websocket_.update_interval_ms);
    LOG_INFO("  AI Server: enabled={}, base_zmq_port={}",
             ai_server_.enabled, ai_server_.base_zmq_port);
    LOG_INFO("  Batch Inference: enabled={}, engine={}, max_batch={}",
             batch_inference_.enabled, batch_inference_.engine_path,
             batch_inference_.max_batch_size);
    LOG_INFO("  Logging: level={}, file={}",
             logging_.level, logging_.file_path);
}

} // namespace vms
