#include "api/device_controller.h"
#include "database/db_manager.h"
#include "utils/api_utils.h"
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

    json results = json::array();
    QSqlRecord rec = query.record();
    while (query.next()) {
        json row;
        for (int i = 0; i < rec.count(); ++i) {
            std::string name = rec.fieldName(i).toStdString();
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

    // GET /api/devices — List all devices
    CROW_ROUTE(app, "/api/devices")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto devices = queryDevices(
            "SELECT id, name, type, ip_address, port, brand, model, username, "
            "channel_count, is_online, firmware, created_at, updated_at "
            "FROM devices ORDER BY created_at DESC");
        return ApiUtils::createResponse({{"devices", devices}}, 200, origin);
    });

    // POST /api/devices — Add device
    CROW_ROUTE(app, "/api/devices")
    .methods(crow::HTTPMethod::POST)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

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

            QSqlDatabase db = database::DbManager::getInstance().getThreadConnection();
            if (!db.isOpen()) return ApiUtils::createErrorResponse("Database not available", 500, origin);

            QSqlQuery query(db);
            const char* sql = "INSERT INTO devices (name, type, ip_address, port, brand, model, username, password, channel_count, is_online, updated_at) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, strftime('%s', 'now'))";

            query.prepare(sql);
            query.bindValue(0, QString::fromStdString(name));
            query.bindValue(1, QString::fromStdString(type));
            query.bindValue(2, QString::fromStdString(ip));
            query.bindValue(3, port);
            query.bindValue(4, QString::fromStdString(brand));
            query.bindValue(5, QString::fromStdString(model));
            query.bindValue(6, QString::fromStdString(username));
            query.bindValue(7, QString::fromStdString(password));
            query.bindValue(8, channel_count);

            if (!query.exec()) {
                return ApiUtils::createErrorResponse("Failed to insert device", 500, origin);
            }

            int new_id = query.lastInsertId().toInt();
            LOG_INFO("[DeviceCtrl] Added device '{}' (ID={}) at {}", name, new_id, ip);

            return ApiUtils::createResponse({
                {"id", new_id}, {"name", name}
            }, 201, origin);

        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // PUT /api/devices/<id> — Update device
    CROW_ROUTE(app, "/api/devices/<int>")
    .methods(crow::HTTPMethod::PUT, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto body = json::parse(req.body);
            QSqlDatabase db = database::DbManager::getInstance().getThreadConnection();
            if (!db.isOpen()) return ApiUtils::createErrorResponse("Database not available", 500, origin);

            auto existing_devices = queryDevices("SELECT ip_address, port, username, password FROM devices WHERE id = ?", {std::to_string(device_id)});
            if (existing_devices.empty()) {
                return ApiUtils::createErrorResponse("Device not found", 404, origin);
            }
            const auto& existing = existing_devices[0];

            std::string ip = body.value("ip_address", existing.value("ip_address", ""));
            int port = body.value("port", existing.value("port", 80));
            std::string username = body.value("username", existing.value("username", ""));
            std::string password = body.value("password", existing.value("password", ""));
            if (body.contains("ip_address") || body.contains("port") || body.contains("username") || body.contains("password")) {
                if (auto validation_error = validateDeviceConnection(ip, port, username, password); validation_error.has_value()) {
                    return ApiUtils::createErrorResponse(validation_error.value(), 400, origin);
                }
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
            addField("password", "password");

            if (body.contains("port")) {
                sets.push_back("port = ?");
                values.push_back(std::to_string(body["port"].get<int>()));
            }
            if (body.contains("channel_count")) {
                sets.push_back("channel_count = ?");
                values.push_back(std::to_string(body["channel_count"].get<int>()));
            }

            sets.push_back("updated_at = strftime('%s', 'now')");

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
            return ApiUtils::createResponse(json::object(), 200, origin);

        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // DELETE /api/devices/<id> — Delete device
    CROW_ROUTE(app, "/api/devices/<int>")
    .methods(crow::HTTPMethod::DELETE)
    ([](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        QSqlDatabase db = database::DbManager::getInstance().getThreadConnection();
        if (!db.isOpen()) return ApiUtils::createErrorResponse("Database not available", 500, origin);

        QSqlQuery del_query(db);
        del_query.prepare("DELETE FROM devices WHERE id = ?");
        del_query.bindValue(0, device_id);
        if (del_query.exec()) {
            LOG_INFO("[DeviceCtrl] Deleted device {}", device_id);
            return ApiUtils::createResponse(json::object(), 200, origin);
        }
        return ApiUtils::createErrorResponse("Failed to delete device", 500, origin);
    });

    // GET /api/devices/<id>/channels — List NVR camera channels
    CROW_ROUTE(app, "/api/devices/<int>/channels")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        // Get device info — credentials fetched server-side only, never exposed in response
        auto devices = queryDevices(
            "SELECT id, name, ip_address, port, brand, model, username, password, channel_count "
            "FROM devices WHERE id = ?",
            {std::to_string(device_id)});
        if (devices.empty()) {
            return ApiUtils::createErrorResponse("Device not found", 404, origin);
        }

        auto& dev = devices[0];
        std::string ip = dev.value("ip_address", "");
        std::string user = dev.value("username", "");
        std::string pass = dev.value("password", "");
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
        // Suppress unused-variable warnings for server-side-only vars
        (void)user; (void)pass;

        return ApiUtils::createResponse({{"channels", channels}, {"device_id", device_id}}, 200, origin);
    });

    // POST /api/devices/<id>/sync — Sync/discover cameras on NVR
    CROW_ROUTE(app, "/api/devices/<int>/sync")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto devices = queryDevices(
            "SELECT id, ip_address, port, brand, username, password "
            "FROM devices WHERE id = ?",
            {std::to_string(device_id)});
        if (devices.empty()) {
            return ApiUtils::createErrorResponse("Device not found", 404, origin);
        }

        auto& dev = devices[0];
        std::string ip = dev.value("ip_address", "");
        int port = dev.value("port", 80);
        std::string user = dev.value("username", "");
        std::string pass = dev.value("password", "");
        std::string brand = dev.value("brand", "");

        // Try to detect channel count via HTTP probe
        int detected_channels = 0;
        bool probe_success = false;
        std::string brand_lower = brand;
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
        std::string update_sql = "UPDATE devices SET channel_count = " + std::to_string(detected_channels) +
                                 ", is_online = " + std::to_string(probe_success ? 1 : 0) +
                                 ", last_seen = strftime('%s', 'now'), updated_at = strftime('%s', 'now') WHERE id = " + std::to_string(device_id);
        database::DbManager::getInstance().execute(update_sql);

        LOG_INFO("[DeviceCtrl] Synced device {} — detected {} channels", device_id, detected_channels);

        return ApiUtils::createResponse({
            {"device_id", device_id},
            {"channel_count", detected_channels}
        }, 200, origin);
    });

    // POST /api/devices/<id>/reboot — Remote reboot
    CROW_ROUTE(app, "/api/devices/<int>/reboot")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto devices = queryDevices("SELECT * FROM devices WHERE id = ?", {std::to_string(device_id)});
        if (devices.empty()) {
            return ApiUtils::createErrorResponse("Device not found", 404, origin);
        }

        auto& dev = devices[0];
        std::string ip = dev.value("ip_address", "");
        int port = dev.value("port", 80);
        std::string user = dev.value("username", "");
        std::string pass = dev.value("password", "");
        std::string brand = dev.value("brand", "");

        bool success = false;
        std::string brand_lower = brand;
        std::transform(brand_lower.begin(), brand_lower.end(), brand_lower.begin(), ::tolower);

        if (brand_lower.find("hikvision") != std::string::npos) {
            auto response = vms::http::putDigest(ip, port, "/ISAPI/System/reboot", user, pass, "", "application/xml", 5L, 10L);
            success = response.ok() || response.body.find("OK") != std::string::npos ||
                      response.body.find("reboot") != std::string::npos;
        } else if (brand_lower.find("dahua") != std::string::npos) {
            auto response = vms::http::getDigest(ip, port, "/cgi-bin/magicBox.cgi?action=reboot", user, pass, 5L, 10L);
            success = response.ok() || response.body.find("OK") != std::string::npos;
        }

        LOG_INFO("[DeviceCtrl] Reboot device {} ({}): {}", device_id, ip, success ? "OK" : "FAILED");

        if (!success) return ApiUtils::createErrorResponse("Reboot failed or unsupported brand", 500, origin);
        return ApiUtils::createResponse({
            {"device_id", device_id}
        }, 200, origin);
    });

    // GET /api/devices/<id>/info — Device info
    CROW_ROUTE(app, "/api/devices/<int>/info")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([](const crow::request& req, int device_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto devices = queryDevices("SELECT * FROM devices WHERE id = ?", {std::to_string(device_id)});
        if (devices.empty()) {
            return ApiUtils::createErrorResponse("Device not found", 404, origin);
        }

        return ApiUtils::createResponse(devices[0], 200, origin);
    });

    LOG_INFO("[DeviceController] Routes registered");
}

} // namespace api
} // namespace vms
