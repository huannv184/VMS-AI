#include "api/device_controller.h"
#include "database/audit_repository.h"
#include "database/db_manager.h"
#include "middleware/auth_middleware.h"
#include "server/vms_app.h"
#include "utils/api_utils.h"
#include "utils/crypto.h"
#include "utils/http_digest_client.h"
#include "utils/logger.h"
#include "utils/validator.h"
#include <nlohmann/json.hpp>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QVariant>
#include <ctime>
#include <optional>
#include <unordered_set>

using json = nlohmann::json;

#ifdef DELETE
#undef DELETE
#endif

namespace vms {
namespace api {

// ============================================================================
// Helper: Execute DB query and return rows as JSON array
// ============================================================================
static bool isValidDeviceHost(const std::string& value) {
    return vms::Validator::isValidIpAddress(value) || vms::Validator::isValidHostname(value);
}

static std::optional<std::string> validateDeviceConnection(const std::string& ip,
                                                           int port,
                                                           const std::string& username,
                                                           const std::string& password) {
    if (!isValidDeviceHost(ip)) {
        return "Invalid ip_address";
    }
    if (!vms::Validator::isValidPort(port)) {
        return "Invalid port";
    }
    if (!vms::Validator::isSafeCredential(username) || !vms::Validator::isSafeCredential(password, 256)) {
        return "Invalid device credentials";
    }
    return std::nullopt;
}

// BUG-C4 FIX: never serialise the encrypted-password column to API clients.
// Even though it is now AES-CBC ciphertext rather than plaintext (see below),
// the envelope format reveals the IV and lets an attacker mount offline
// dictionary attacks if they capture the response. Internal callers that need
// the cleartext (e.g. RTSP probing) must read it directly from the DB and
// decrypt via Crypto::decrypt — they should not go through this serializer.
static const std::unordered_set<std::string>& sensitiveDeviceColumns() {
    static const std::unordered_set<std::string> cols = { "password" };
    return cols;
}

// BUG-C4 regression FIX: callers that need the cleartext password for digest
// auth (reboot, channel-probe sync) must read the column directly and decrypt
// it — `queryDevices()` deliberately strips the `password` column from its
// JSON output. Going through that helper meant `pass` was always empty and
// every digest call landed on the device with empty Authorization → 401.
struct DeviceConnection {
    std::string ip;
    int port = 80;
    std::string username;
    std::string password; // cleartext, server-side only
    std::string brand;
    int channel_count = 0;
    bool found = false;
};

static DeviceConnection loadDeviceConnection(int device_id) {
    DeviceConnection conn;
    QSqlDatabase db = database::DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return conn;

    QSqlQuery q(db);
    q.prepare("SELECT ip_address, port, username, password, brand, channel_count "
              "FROM devices WHERE id = ?");
    q.bindValue(0, device_id);
    if (!q.exec() || !q.next()) return conn;

    conn.ip       = q.value(0).toString().toStdString();
    conn.port     = q.value(1).toInt();
    if (conn.port <= 0) conn.port = 80;
    conn.username = q.value(2).toString().toStdString();
    std::string stored_pwd = q.value(3).toString().toStdString();
    conn.brand    = q.value(4).toString().toStdString();
    conn.channel_count = q.value(5).toInt();

    if (!stored_pwd.empty()) {
        try {
            conn.password = vms::utils::Crypto::decrypt(stored_pwd);
        } catch (const std::exception& e) {
            // Pre-C4 rows are still cleartext; treat decrypt failure as cleartext
            // pass-through so existing devices keep working until rotated.
            LOG_WARN("[DeviceCtrl] decrypt failed for device {} ({}); using stored value as cleartext", device_id, e.what());
            conn.password = stored_pwd;
        }
    }
    conn.found = true;
    return conn;
}

static json queryDevices(const std::string& sql, const std::vector<std::string>& params = {}) {
    QSqlDatabase db = database::DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return json::array();

    QSqlQuery query(db);
    query.prepare(QString::fromStdString(sql));
    for (size_t i = 0; i < params.size(); ++i) {
        query.bindValue(static_cast<int>(i), QString::fromStdString(params[i]));
    }
    if (!query.exec()) {
        LOG_ERROR("[DeviceCtrl] SQL failed: {}", query.lastError().text().toStdString());
        return json::array();
    }

    const auto& redacted = sensitiveDeviceColumns();
    json results = json::array();
    QSqlRecord rec = query.record();
    while (query.next()) {
        json row;
        for (int i = 0; i < rec.count(); ++i) {
            std::string name = rec.fieldName(i).toStdString();
            if (redacted.count(name)) continue; // skip ciphertext, never expose
            QVariant val = query.value(i);
            if (val.isNull()) { row[name] = nullptr; }
            else if (val.typeId() == QMetaType::LongLong || val.typeId() == QMetaType::Int) { row[name] = val.toLongLong(); }
            else if (val.typeId() == QMetaType::Double) { row[name] = val.toDouble(); }
            else { row[name] = val.toString().toStdString(); }
        }
        results.push_back(row);
    }
    return results;
}

void DeviceController::registerRoutes(vms::server::VmsApp& app) {

    // GET /api/devices — List all devices (DEVICE_READ)
    CROW_ROUTE(app, "/api/devices")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::DEVICE_READ, origin)) return std::move(*err);

        auto devices = queryDevices(
            "SELECT id, name, type, ip_address, port, brand, model, username, "
            "channel_count, is_online, firmware, created_at, updated_at "
            "FROM devices ORDER BY created_at DESC");
        return ApiUtils::createResponse({{"devices", devices}}, 200, origin);
    });

    // POST /api/devices — Add device (DEVICE_WRITE)
    CROW_ROUTE(app, "/api/devices")
    .methods(crow::HTTPMethod::POST)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::DEVICE_WRITE, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            std::string name = body.value("name", "");
            std::string type = body.value("type", "NVR");
            std::string ip   = body.value("ip_address", "");
            int port         = body.value("port", 80);
            std::string brand    = body.value("brand", "");
            std::string model    = body.value("model", "");
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            int channel_count    = body.value("channel_count", 0);

            if (name.empty() || ip.empty()) {
                return ApiUtils::createErrorResponse("name and ip_address are required", 400, origin);
            }
            if (auto validation_error = validateDeviceConnection(ip, port, username, password); validation_error.has_value()) {
                return ApiUtils::createErrorResponse(validation_error.value(), 400, origin);
            }

            auto& db_mgr = database::DbManager::getInstance();
            QSqlDatabase db = db_mgr.getThreadConnection();
            if (!db.isOpen()) return ApiUtils::createErrorResponse("Database not available", 500, origin);

            QSqlQuery query(db);
            // strftime('%s','now') is SQLite-only — go through sqlNowEpoch()
            // so the same INSERT runs on PostgreSQL too.
            const std::string sql_str =
                "INSERT INTO devices (name, type, ip_address, port, brand, model, username, password, channel_count, is_online, updated_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, " + db_mgr.sqlNowEpoch() + ")";

            // BUG-C4 FIX: encrypt password at rest. Stored as "v1:<iv_b64>:<ct_b64>"
            // and decrypted on use via Crypto::decrypt. Empty password is left empty
            // so we don't store noise rows.
            std::string stored_password = password.empty()
                ? std::string{}
                : vms::utils::Crypto::encrypt(password);

            query.prepare(QString::fromStdString(sql_str));
            query.bindValue(0, QString::fromStdString(name));
            query.bindValue(1, QString::fromStdString(type));
            query.bindValue(2, QString::fromStdString(ip));
            query.bindValue(3, port);
            query.bindValue(4, QString::fromStdString(brand));
            query.bindValue(5, QString::fromStdString(model));
            query.bindValue(6, QString::fromStdString(username));
            query.bindValue(7, QString::fromStdString(stored_password));
            query.bindValue(8, channel_count);

            if (!query.exec()) {
                return ApiUtils::createErrorResponse("Failed to insert device", 500, origin);
            }

            int new_id = query.lastInsertId().toInt();
            LOG_INFO("[DeviceCtrl] Added device '{}' (ID={}) at {}", name, new_id, ip);

            database::AuditRepository audit;
            audit.insertLog(ctx.user->id, "CREATE_DEVICE",
                            "Created device '" + name + "' (id=" + std::to_string(new_id) + ", ip=" + ip + ")");

            return ApiUtils::createResponse({
                {"id", new_id}, {"name", name}
            }, 201, origin);

        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // PUT /api/devices/<id> — Update device (DEVICE_WRITE)
    CROW_ROUTE(app, "/api/devices/<int>")
    .methods(crow::HTTPMethod::PUT, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::DEVICE_WRITE, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            auto& db_mgr = database::DbManager::getInstance();
            QSqlDatabase db = db_mgr.getThreadConnection();
            if (!db.isOpen()) return ApiUtils::createErrorResponse("Database not available", 500, origin);

            // BUG-C4 FIX: queryDevices() now strips the password column, so we
            // can no longer pull the existing password through it. Use the body
            // value if supplied; otherwise validate connection without needing
            // to know the existing cleartext (we only check fields actually being
            // changed).
            auto existing_devices = queryDevices("SELECT ip_address, port, username FROM devices WHERE id = ?", {std::to_string(device_id)});
            if (existing_devices.empty()) {
                return ApiUtils::createErrorResponse("Device not found", 404, origin);
            }
            const auto& existing = existing_devices[0];

            std::string ip = body.value("ip_address", existing.value("ip_address", ""));
            int port = body.value("port", existing.value("port", 80));
            std::string username = body.value("username", existing.value("username", ""));
            // Only validate password format when the request actually changes it.
            if (body.contains("password") && body["password"].is_string()) {
                std::string password = body["password"].get<std::string>();
                if (auto validation_error = validateDeviceConnection(ip, port, username, password); validation_error.has_value()) {
                    return ApiUtils::createErrorResponse(validation_error.value(), 400, origin);
                }
            } else if (body.contains("ip_address") || body.contains("port") || body.contains("username")) {
                // No password change but other connection fields moved — re-validate
                // those without needing the cleartext password.
                if (!isValidDeviceHost(ip)) return ApiUtils::createErrorResponse("Invalid ip_address", 400, origin);
                if (!vms::Validator::isValidPort(port)) return ApiUtils::createErrorResponse("Invalid port", 400, origin);
                if (!vms::Validator::isSafeCredential(username)) return ApiUtils::createErrorResponse("Invalid username", 400, origin);
            }

            // Build dynamic UPDATE
            std::vector<std::string> sets;
            std::vector<std::string> values;

            auto addField = [&](const std::string& field, const std::string& key) {
                if (body.contains(key)) {
                    sets.push_back(field + " = ?");
                    if (body[key].is_string()) values.push_back(body[key].get<std::string>());
                    else values.push_back(body[key].dump());
                }
            };

            addField("name", "name");
            addField("type", "type");
            addField("ip_address", "ip_address");
            addField("brand", "brand");
            addField("model", "model");
            addField("firmware", "firmware");
            addField("username", "username");
            // BUG-C4 FIX: encrypt the password before persisting on UPDATE too.
            if (body.contains("password") && body["password"].is_string()) {
                std::string raw_pwd = body["password"].get<std::string>();
                sets.push_back("password = ?");
                values.push_back(raw_pwd.empty() ? std::string{} : vms::utils::Crypto::encrypt(raw_pwd));
            }

            if (body.contains("port")) {
                sets.push_back("port = ?");
                values.push_back(std::to_string(body["port"].get<int>()));
            }
            if (body.contains("channel_count")) {
                sets.push_back("channel_count = ?");
                values.push_back(std::to_string(body["channel_count"].get<int>()));
            }

            // updated_at via sqlNowEpoch() so the same UPDATE runs on PG/SQLite.
            sets.push_back("updated_at = " + db_mgr.sqlNowEpoch());

            std::string sql = "UPDATE devices SET ";
            for (size_t i = 0; i < sets.size(); ++i) {
                if (i > 0) sql += ", ";
                sql += sets[i];
            }
            sql += " WHERE id = ?";
            values.push_back(std::to_string(device_id));

            QSqlQuery query(db);
            query.prepare(QString::fromStdString(sql));
            for (size_t i = 0; i < values.size(); ++i) {
                query.bindValue(static_cast<int>(i), QString::fromStdString(values[i]));
            }

            if (!query.exec()) {
                return ApiUtils::createErrorResponse("Failed to update device", 500, origin);
            }

            LOG_INFO("[DeviceCtrl] Updated device {}", device_id);
            database::AuditRepository audit;
            audit.insertLog(ctx.user->id, "UPDATE_DEVICE",
                            "Updated device id=" + std::to_string(device_id));
            return ApiUtils::createResponse(json::object(), 200, origin);

        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // DELETE /api/devices/<id> — Delete device (DEVICE_WRITE)
    CROW_ROUTE(app, "/api/devices/<int>")
    .methods(crow::HTTPMethod::DELETE)
    ([&app](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::DEVICE_WRITE, origin)) return std::move(*err);

        QSqlDatabase db = database::DbManager::getInstance().getThreadConnection();
        if (!db.isOpen()) return ApiUtils::createErrorResponse("Database not available", 500, origin);

        QSqlQuery del_query(db);
        del_query.prepare("DELETE FROM devices WHERE id = ?");
        del_query.bindValue(0, device_id);
        if (del_query.exec()) {
            LOG_INFO("[DeviceCtrl] Deleted device {}", device_id);
            database::AuditRepository audit;
            audit.insertLog(ctx.user->id, "DELETE_DEVICE",
                            "Deleted device id=" + std::to_string(device_id));
            return ApiUtils::createResponse(json::object(), 200, origin);
        }
        return ApiUtils::createErrorResponse("Failed to delete device", 500, origin);
    });

    // GET /api/devices/<id>/channels — List NVR camera channels (DEVICE_READ)
    CROW_ROUTE(app, "/api/devices/<int>/channels")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::DEVICE_READ, origin)) return std::move(*err);

        // /channels only generates URL paths from brand+channel_count — no
        // credentials needed here. Keep the route free of decrypt overhead.
        auto devices = queryDevices(
            "SELECT id, name, ip_address, port, brand, model, username, channel_count "
            "FROM devices WHERE id = ?",
            {std::to_string(device_id)});
        if (devices.empty()) {
            return ApiUtils::createErrorResponse("Device not found", 404, origin);
        }

        auto& dev = devices[0];
        std::string brand = dev.value("brand", "");
        int channel_count = dev.value("channel_count", 16);

        // Generate channel list — strip credentials from returned RTSP URLs (C3)
        json channels = json::array();
        std::string brand_lower = brand;
        std::transform(brand_lower.begin(), brand_lower.end(), brand_lower.begin(), ::tolower);

        for (int ch = 1; ch <= channel_count; ++ch) {
            json channel;
            channel["channel"] = ch;
            channel["name"] = "Channel " + std::to_string(ch);

            // Credentials are kept server-side; clients use the camera proxy endpoints
            if (brand_lower.find("hikvision") != std::string::npos || brand_lower.find("hik") != std::string::npos) {
                channel["rtsp_path"] = "/Streaming/Channels/" + std::to_string(ch * 100 + 1);
                channel["rtsp_sub_path"] = "/Streaming/Channels/" + std::to_string(ch * 100 + 2);
            } else if (brand_lower.find("dahua") != std::string::npos) {
                channel["rtsp_path"] = "/cam/realmonitor?channel=" + std::to_string(ch) + "&subtype=0";
                channel["rtsp_sub_path"] = "/cam/realmonitor?channel=" + std::to_string(ch) + "&subtype=1";
            } else {
                channel["rtsp_path"] = "/stream" + std::to_string(ch);
                channel["rtsp_sub_path"] = "";
            }

            channel["brand"] = brand;
            channels.push_back(channel);
        }

        return ApiUtils::createResponse({{"channels", channels}, {"device_id", device_id}}, 200, origin);
    });

    // POST /api/devices/<id>/sync — Sync/discover cameras on NVR (DEVICE_WRITE)
    CROW_ROUTE(app, "/api/devices/<int>/sync")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::DEVICE_WRITE, origin)) return std::move(*err);

        auto conn = loadDeviceConnection(device_id);
        if (!conn.found) {
            return ApiUtils::createErrorResponse("Device not found", 404, origin);
        }
        const std::string& ip = conn.ip;
        int port = conn.port;
        const std::string& user = conn.username;
        const std::string& pass = conn.password;

        // Try to detect channel count via HTTP probe
        int detected_channels = 0;
        bool probe_success = false;
        std::string brand_lower = conn.brand;
        std::transform(brand_lower.begin(), brand_lower.end(), brand_lower.begin(), ::tolower);

        if (brand_lower.find("hikvision") != std::string::npos) {
            auto response = vms::http::getDigest(ip, port, "/ISAPI/ContentMgmt/InputProxy/channels", user, pass, 3L, 5L);
            if (response.status_code >= 0) {
                const auto& result = response.body;
                probe_success = response.ok();
                // Count <InputProxyChannel> occurrences
                size_t pos = 0;
                while ((pos = result.find("<InputProxyChannel>", pos)) != std::string::npos) {
                    detected_channels++;
                    pos++;
                }
                // Fallback: count via VideoInput channels
                if (detected_channels == 0) {
                    pos = 0;
                    while ((pos = result.find("<VideoInputChannel>", pos)) != std::string::npos) {
                        detected_channels++;
                        pos++;
                    }
                }
            }
        } else if (brand_lower.find("dahua") != std::string::npos) {
            auto response = vms::http::getDigest(
                ip,
                port,
                "/cgi-bin/magicBox.cgi?action=getProductDefinition&name=MaxVideoInputChannels",
                user,
                pass,
                3L,
                5L
            );
            if (response.status_code >= 0) {
                const auto& result = response.body;
                probe_success = response.ok();
                // Parse "table.MaxVideoInputChannels=16"
                auto eq = result.find('=');
                if (eq != std::string::npos) {
                    try { detected_channels = std::stoi(result.substr(eq + 1)); } catch (...) {}
                }
            }
        }

        if (detected_channels <= 0) detected_channels = 16; // Fallback

        // Update device with detected channel count and mark online
        auto& sync_db_mgr = database::DbManager::getInstance();
        const std::string now_expr = sync_db_mgr.sqlNowEpoch();
        std::string update_sql = "UPDATE devices SET channel_count = " + std::to_string(detected_channels) +
                                 ", is_online = " + std::to_string(probe_success ? 1 : 0) +
                                 ", last_seen = " + now_expr + ", updated_at = " + now_expr +
                                 " WHERE id = " + std::to_string(device_id);
        sync_db_mgr.execute(update_sql);

        LOG_INFO("[DeviceCtrl] Synced device {} — detected {} channels", device_id, detected_channels);

        database::AuditRepository audit;
        audit.insertLog(ctx.user->id, "SYNC_DEVICE",
                        "Synced device id=" + std::to_string(device_id) +
                        ", channels=" + std::to_string(detected_channels));

        return ApiUtils::createResponse({
            {"device_id", device_id},
            {"channel_count", detected_channels}
        }, 200, origin);
    });

    // POST /api/devices/<id>/reboot — Remote reboot
    // Reboot is more dangerous than write — limit to SYSTEM_ADMIN.
    // An operator (role 2) holding DEVICE_WRITE can still configure/sync, but
    // can't physically restart an NVR/DVR while it's recording on a customer
    // site.
    CROW_ROUTE(app, "/api/devices/<int>/reboot")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requireAdmin(ctx, origin)) return std::move(*err);

        auto conn = loadDeviceConnection(device_id);
        if (!conn.found) {
            return ApiUtils::createErrorResponse("Device not found", 404, origin);
        }

        bool success = false;
        std::string brand_lower = conn.brand;
        std::transform(brand_lower.begin(), brand_lower.end(), brand_lower.begin(), ::tolower);

        if (brand_lower.find("hikvision") != std::string::npos) {
            auto response = vms::http::putDigest(conn.ip, conn.port, "/ISAPI/System/reboot",
                                                 conn.username, conn.password, "", "application/xml", 5L, 10L);
            success = response.ok() || response.body.find("OK") != std::string::npos ||
                      response.body.find("reboot") != std::string::npos;
        } else if (brand_lower.find("dahua") != std::string::npos) {
            auto response = vms::http::getDigest(conn.ip, conn.port, "/cgi-bin/magicBox.cgi?action=reboot",
                                                 conn.username, conn.password, 5L, 10L);
            success = response.ok() || response.body.find("OK") != std::string::npos;
        }

        LOG_INFO("[DeviceCtrl] Reboot device {} ({}): {}", device_id, conn.ip, success ? "OK" : "FAILED");

        database::AuditRepository audit;
        audit.insertLog(ctx.user->id, "REBOOT_DEVICE",
                        "Reboot device id=" + std::to_string(device_id) +
                        " ip=" + conn.ip +
                        " result=" + (success ? "OK" : "FAILED"));

        if (!success) return ApiUtils::createErrorResponse("Reboot failed or unsupported brand", 500, origin);
        return ApiUtils::createResponse({
            {"device_id", device_id}
        }, 200, origin);
    });

    // GET /api/devices/<id>/info — Device info (DEVICE_READ)
    CROW_ROUTE(app, "/api/devices/<int>/info")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::DEVICE_READ, origin)) return std::move(*err);

        // BUG-C4 FIX: explicitly list non-sensitive columns instead of SELECT *.
        // Even with the queryDevices serialiser stripping `password`, defence in
        // depth: don't fetch ciphertext over the wire from DB at all when we
        // never intend to send it back.
        auto devices = queryDevices(
            "SELECT id, name, type, ip_address, port, brand, model, username, "
            "channel_count, is_online, firmware, created_at, updated_at "
            "FROM devices WHERE id = ?",
            {std::to_string(device_id)});
        if (devices.empty()) {
            return ApiUtils::createErrorResponse("Device not found", 404, origin);
        }

        return ApiUtils::createResponse(devices[0], 200, origin);
    });

    LOG_INFO("[DeviceController] Routes registered");
}

} // namespace api
} // namespace vms
