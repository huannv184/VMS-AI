#include "streaming/camera_stream_manager_qt.h"
#include "core/camera_pipeline_manager.h"
#include "utils/logger.h"
#include "utils/config.h"
#include "utils/jwt_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>
#include <atomic>
#include <chrono>
#include <deque>
#include <unordered_set>
#include <mutex>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QAbstractSocket>

namespace vms {
namespace streaming {

// Protocol Constants
constexpr uint32_t MAGIC_BYTES = 0x564D5331; // 'VMS1'
constexpr uint8_t PROTOCOL_VERSION = 3;
constexpr uint8_t HEADER_LENGTH_V2 = 20;
constexpr uint8_t FRAME_TYPE_FMP4 = 3;

// Helper to write Big-Endian (for protocol compatibility)
static void write_u32_be(QByteArray& arr, uint32_t v) {
    arr.append(static_cast<char>((v >> 24) & 0xFF));
    arr.append(static_cast<char>((v >> 16) & 0xFF));
    arr.append(static_cast<char>((v >>  8) & 0xFF));
    arr.append(static_cast<char>( v        & 0xFF));
}

static void write_u8(QByteArray& arr, uint8_t v) {
    arr.append(static_cast<char>(v));
}

// ============================================================
// Singleton
// ============================================================

CameraStreamManager& CameraStreamManager::getInstance() {
    static CameraStreamManager instance;
    return instance;
}

CameraStreamManager::CameraStreamManager() {
    LOG_INFO("CameraStreamManager initialized (Qt QWebSocketServer mode)");
}

CameraStreamManager::~CameraStreamManager() {
    stop();
}

// ============================================================
// Lifecycle
// ============================================================

void CameraStreamManager::start() {
    if (server_) return;

    int port = vms::Config::getInstance().getWebSocketConfig().port;
    if (port <= 0) {
        LOG_INFO("QWebSocketServer disabled (websocket.port={})", port);
        return;
    }
    server_ = new QWebSocketServer(QStringLiteral("VMS Video Stream Server"),
                                  QWebSocketServer::NonSecureMode, this);

    constexpr int MAX_PORT_FALLBACK = 10;
    int bound_port = -1;
    for (int attempt = 0; attempt < MAX_PORT_FALLBACK; ++attempt) {
        int try_port = port + attempt;
        if (server_->listen(QHostAddress::Any, static_cast<quint16>(try_port))) {
            bound_port = try_port;
            break;
        }
        LOG_WARN("QWebSocketServer port {} in use ({}), trying {}",
                 try_port, server_->errorString().toStdString(), try_port + 1);
    }

    if (bound_port > 0) {
        bound_port_ = bound_port; // publish actual port for API consumers
        if (bound_port != port) {
            LOG_WARN("QWebSocketServer listening on fallback port {} (configured: {})", bound_port, port);
        } else {
            LOG_INFO("QWebSocketServer listening on port {}", bound_port);
        }
        connect(server_, &QWebSocketServer::newConnection, this, &CameraStreamManager::onNewConnection);
    } else {
        LOG_ERROR("QWebSocketServer failed to bind on ports {}-{}: {}",
                  port, port + MAX_PORT_FALLBACK - 1, server_->errorString().toStdString());
    }
}

void CameraStreamManager::stop() {
    // H8: QWebSocketServer must be closed and deleted on the Qt thread that owns it.
    // If stop() is called from a non-Qt thread (signal handler, Crow worker), marshal
    // the call through the event loop using BlockingQueuedConnection so the caller
    // waits for the teardown to complete before returning.
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, &CameraStreamManager::stop,
                                  Qt::BlockingQueuedConnection);
        return;
    }

    // Now we are on the Qt thread — safe to call Qt methods directly.
    if (server_) {
        server_->close();
        server_->deleteLater(); // deferred Qt-safe deletion
        server_.clear();
    }

    std::lock_guard<std::shared_mutex> lock(camera_mutex_);
    camera_clients_.clear();
    latest_fmp4_init_packets_.clear();
}

// ============================================================
// Connections & Handlers
// ============================================================

void CameraStreamManager::onNewConnection() {
    QPointer<QWebSocket> socket = server_->nextPendingConnection();
    if (!socket) {
        return;
    }
    LOG_THROTTLED_INFO(5000, "New WS connection from: {} (Path: {})",
             socket->peerAddress().toString().toStdString(),
             socket->requestUrl().path().toStdString());

    connect(socket.data(), &QWebSocket::textMessageReceived, this, &CameraStreamManager::processTextMessage);
    connect(socket.data(), &QWebSocket::binaryMessageReceived, this, &CameraStreamManager::processBinaryMessage);
    connect(socket.data(), &QWebSocket::disconnected, this, &CameraStreamManager::socketDisconnected);

    // Disable Nagle's algorithm: WebSocket frames are self-framed and latency-sensitive.
    // Without TCP_NODELAY, the kernel may buffer small writes up to 40ms before sending.
    if (auto* tcp = socket->findChild<QAbstractSocket*>()) {
        tcp->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    }

    // Default: unauthenticated until AUTH message is received (when auth is enabled)
    socket->setProperty("vms_authed", false);

    // 2026-05-15 WS audit: pre-auth socket timeout. Pre-fix an attacker could
    // open N sockets, never send AUTH, and hold resources indefinitely (file
    // descriptors + QWebSocket per-conn buffers + the camera_clients_ entry
    // never came into play but the QWebSocket itself remained). Now: a 10s
    // single-shot timer per socket force-closes any connection still in the
    // unauthenticated state when it fires. Timer is parented to the socket
    // so it auto-deletes with the socket on disconnect; no manual stop()
    // needed on AUTH success because the lambda no-ops if vms_authed=true.
    // Auth-disabled mode (config.auth.enabled=false) marks the socket
    // authed on the first AUTH-or-other message — timer still fires but
    // is a no-op then. 10s is generous: a normal browser+JS client takes
    // <1s from connect to first AUTH frame; 10s tolerates slow links +
    // operator manual-connect with token paste.
    auto* auth_timeout = new QTimer(socket.data());
    auth_timeout->setSingleShot(true);
    auth_timeout->setInterval(10000);
    QObject::connect(auth_timeout, &QTimer::timeout, socket.data(), [socket]() {
        if (!socket || socket->property("vms_authed").toBool()) return;
        LOG_THROTTLED_WARN(5000,
            "WS pre-auth timeout: closing {} after 10s with no AUTH",
            socket->peerAddress().toString().toStdString());
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                      QStringLiteral("AUTH timeout"));
    });
    auth_timeout->start();
}

void CameraStreamManager::socketDisconnected() {
    QPointer<QWebSocket> socket = qobject_cast<QWebSocket*>(sender());
    if (socket) {
        LOG_THROTTLED_INFO(5000, "WS client disconnected: {}", socket->peerAddress().toString().toStdString());

        removeClient(socket);
        socket->deleteLater();
    }
}

void CameraStreamManager::handleAiEvent(const QString& type, const QString& payload) {
    try {
        auto j = nlohmann::json::parse(payload.toStdString());
        // Forward AI event to camera 0 (global) and specific camera if available
        int cam_id = j.value("camera_id", 0);
        broadcastEvent(cam_id, j);
    } catch (const std::exception& e) {
        LOG_THROTTLED_ERROR(5000, "handleAiEvent: failed to parse AI payload: {}", e.what());
    }
}

void CameraStreamManager::processTextMessage(const QString& message) {
    QPointer<QWebSocket> socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;

    try {
        auto j = nlohmann::json::parse(message.toStdString());

        const bool auth_enabled = vms::Config::getInstance().getAuthConfig().enabled;
        const bool is_authed = socket->property("vms_authed").toBool();

        // AUTH handshake: client should send {type:"AUTH", token:"..."} once after onopen
        if (j.contains("type") && j["type"].is_string() && j["type"] == "AUTH") {
            if (!auth_enabled) {
                socket->setProperty("vms_authed", true);
                sendText(socket, QStringLiteral("{\"type\":\"AUTH_OK\"}"));
                return;
            }
            if (!j.contains("token") || !j["token"].is_string()) {
                sendText(socket, QStringLiteral("{\"type\":\"AUTH_ERR\",\"error\":\"token required\"}"));
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("AUTH required"));
                return;
            }
            vms::core::User user;
            std::string jti;
            std::string client_ip = socket->peerAddress().toString().toStdString();
            
            if (vms::utils::decodeWsTicketJwt(j["token"].get<std::string>(), user, jti, client_ip)) {
                // One-time-use ticket enforcement with bounded TTL eviction.
                // deque front = oldest; entries older than 30s are evicted on each auth,
                // so memory is bounded by (max_connections_per_30s) entries — never spikes.
                struct TicketEntry { std::string jti; std::chrono::steady_clock::time_point expires; };
                static std::deque<TicketEntry> ticket_queue;
                static std::unordered_set<std::string> ticket_set;
                static std::mutex ticket_mutex;
                static constexpr auto kTicketTTL = std::chrono::seconds(30);

                std::lock_guard<std::mutex> lock(ticket_mutex);
                auto now = std::chrono::steady_clock::now();
                while (!ticket_queue.empty() && ticket_queue.front().expires <= now) {
                    ticket_set.erase(ticket_queue.front().jti);
                    ticket_queue.pop_front();
                }

                if (ticket_set.count(jti)) {
                    LOG_WARN("WS Ticket replay attack detected for JTI: {}", jti);
                    sendText(socket, QStringLiteral("{\"type\":\"AUTH_ERR\",\"error\":\"ticket already used\"}"));
                    socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("Ticket replay"));
                    return;
                }
                ticket_set.insert(jti);
                ticket_queue.push_back({jti, now + kTicketTTL});

                socket->setProperty("vms_authed", true);
                sendText(socket, QStringLiteral("{\"type\":\"AUTH_OK\"}"));
            } else {
                sendText(socket, QStringLiteral("{\"type\":\"AUTH_ERR\",\"error\":\"invalid, expired, or stolen ticket\"}"));
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("Invalid ticket"));
            }
            return;
        }

        // Enforce auth for all other messages when enabled
        if (auth_enabled && !is_authed) {
            sendText(socket, QStringLiteral("{\"type\":\"AUTH_ERR\",\"error\":\"AUTH required\"}"));
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("AUTH required"));
            return;
        }

        // Stream subscription (backward compatibility)
        if (j.contains("camera_id")) {
            int cam_id = j["camera_id"].get<int>();
            removeClient(socket); // Unsubscribe from previous
            addClient(cam_id, socket);
            
            // Send ack
            nlohmann::json ack = {
                {"type", "subscribed"},
                {"camera_id", cam_id},
                {"status", "ok"}
            };
            sendText(socket, QString::fromStdString(ack.dump()));
        }
        else if (j.contains("type") && j["type"] == "ping") {
            sendText(socket, QStringLiteral("{\"type\":\"pong\"}"));
        }
    } catch (...) {
        // Silent ignore
    }
}

void CameraStreamManager::processBinaryMessage(const QByteArray& message) {
    // We don't expect data from clients for now
}

// ============================================================
// Subscription / Unsubscription
// ============================================================

void CameraStreamManager::addClient(int camera_id, const QPointer<QWebSocket>& client) {
    if (!client) {
        return;
    }

    QByteArray init_packet;
    {
        std::lock_guard<std::shared_mutex> lock(camera_mutex_);
        auto& clients = camera_clients_[camera_id];
        clients.erase(
            std::remove_if(clients.begin(), clients.end(),
                           [&client](const ClientInfo& existing) {
                               return !existing.socket || existing.socket == client;
                           }),
            clients.end());
        clients.push_back({client, std::make_shared<std::atomic<int>>(0)});
        socket_to_camera_[client.data()] = camera_id;

        auto it = latest_fmp4_init_packets_.find(camera_id);
        if (it != latest_fmp4_init_packets_.end()) {
            init_packet = it->second;
        }
        
        LOG_THROTTLED_INFO(5000, "Client subscribed to Camera {} (Total: {})",
                 camera_id, clients.size());
    }

    if (!init_packet.isEmpty()) {
        sendBinary(client, init_packet);
    }
}

void CameraStreamManager::removeClient(int camera_id, const QPointer<QWebSocket>& client) {
    std::lock_guard<std::shared_mutex> lock(camera_mutex_);
    if (client) socket_to_camera_.erase(client.data());

    auto it = camera_clients_.find(camera_id);
    if (it == camera_clients_.end()) return;

    auto& clients = it->second;
    clients.erase(
        std::remove_if(clients.begin(), clients.end(),
                       [&client](const ClientInfo& existing) {
                           return !existing.socket || existing.socket == client;
                       }),
        clients.end());
    if (clients.empty()) {
        camera_clients_.erase(it);
    }
}

void CameraStreamManager::removeClient(QWebSocket* client) {
    removeClient(QPointer<QWebSocket>(client));
}

void CameraStreamManager::removeClient(const QPointer<QWebSocket>& client) {
    if (!client) return;

    std::lock_guard<std::shared_mutex> lock(camera_mutex_);

    // O(1): look up which camera this socket subscribed to.
    auto map_it = socket_to_camera_.find(client.data());
    if (map_it == socket_to_camera_.end()) return; // not subscribed to any camera

    int cam_id = map_it->second;
    socket_to_camera_.erase(map_it);

    auto cam_it = camera_clients_.find(cam_id);
    if (cam_it == camera_clients_.end()) return;

    auto& clients = cam_it->second;
    clients.erase(
        std::remove_if(clients.begin(), clients.end(),
                       [&client](const ClientInfo& existing) {
                           return !existing.socket || existing.socket == client;
                       }),
        clients.end());
    if (clients.empty()) {
        camera_clients_.erase(cam_it);
    }
}

// ============================================================
// Utility / Bridge
// ============================================================

bool CameraStreamManager::isStreaming(int camera_id) {
    return vms::core::CameraPipelineManager::getInstance().isRunning(camera_id);
}

bool CameraStreamManager::startStream(int camera_id, const std::string& rtsp_url) {
    return vms::core::CameraPipelineManager::getInstance().startPipeline(camera_id, rtsp_url);
}

size_t CameraStreamManager::getClientCount(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(camera_mutex_);
    auto it = camera_clients_.find(camera_id);
    return (it != camera_clients_.end()) ? it->second.size() : 0;
}

std::vector<CameraStreamManager::ClientInfo> CameraStreamManager::snapshotClients(int camera_id) {
    std::shared_lock<std::shared_mutex> lock(camera_mutex_);
    auto it = camera_clients_.find(camera_id);
    if (it == camera_clients_.end()) return {};
    std::vector<ClientInfo> clients;
    clients.reserve(it->second.size());
    for (const auto& client : it->second) {
        if (client.socket) {
            clients.push_back(client);
        }
    }
    return clients;
}

// ============================================================
// Send Helpers (Thread Safe via QueuedConnection)
// ============================================================

bool CameraStreamManager::sendBinary(const QPointer<QWebSocket>& client, const QByteArray& data) {
    if (!client) return false;
    QMetaObject::invokeMethod(client.data(), [s = client, data]() {
        if (!s) {
            return;
        }
        s->sendBinaryMessage(data);
    }, Qt::QueuedConnection);
    return true;
}

bool CameraStreamManager::sendBinaryDropIfBusy(const ClientInfo& client, const QByteArray& data, bool is_mandatory) {
    if (!client.socket) return false;

    // Mandatory frames (keyframes / init segments) are always sent.
    if (is_mandatory) {
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        return sendBinary(client.socket, data);
    }

    const int inflight = client.inflight_counter->fetch_add(
        static_cast<int>(data.size()), std::memory_order_relaxed);

    if (inflight > kBackpressureThreshold) {
        client.inflight_counter->fetch_sub(static_cast<int>(data.size()), std::memory_order_relaxed);
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        LOG_THROTTLED_WARN(5000, "Dropped {} frames (backpressure)",
                           frames_dropped_.load(std::memory_order_relaxed));
        return false;
    }

    frames_sent_.fetch_add(1, std::memory_order_relaxed);

    // Queue the send on the Qt thread; decrement counter after completion.
    const int sz = static_cast<int>(data.size());
    QMetaObject::invokeMethod(client.socket.data(), [s = client.socket, data, inflight_counter = client.inflight_counter, sz]() {
        // Always decrement — if socket was deleted the counter must still be released
        // to prevent unbounded growth that would permanently block future frames.
        if (s) {
            s->sendBinaryMessage(data);
        }
        inflight_counter->fetch_sub(sz, std::memory_order_relaxed);
    }, Qt::QueuedConnection);
    return true;
}

bool CameraStreamManager::sendText(const QPointer<QWebSocket>& client, const QString& msg) {
    if (!client) return false;
    QMetaObject::invokeMethod(client.data(), [s = client, msg]() {
        if (!s) {
            return;
        }
        s->sendTextMessage(msg);
    }, Qt::QueuedConnection);
    return true;
}

// ============================================================
// Broadcast Methods
// ============================================================

void CameraStreamManager::broadcastRawFrame(
    int camera_id,
    const std::vector<unsigned char>& jpeg_data,
    const std::vector<inference::TrackedObject>& objects,
    int frame_width,
    int frame_height
) {
    // Early-exit without taking any lock if no subscribers.
    {
        std::shared_lock<std::shared_mutex> lock(camera_mutex_);
        if (camera_clients_.find(camera_id) == camera_clients_.end()) return;
    }

    nlohmann::json meta_json;
    meta_json["cam_id"] = camera_id;
    meta_json["width"]  = frame_width > 0 ? frame_width : 640;
    meta_json["height"] = frame_height > 0 ? frame_height : 360;

    nlohmann::json obj_arr = nlohmann::json::array();
    for (const auto& obj : objects) {
        obj_arr.push_back({
            {"label", obj.label},
            {"confidence", obj.confidence},
            {"bbox", {obj.bbox.x1, obj.bbox.y1, obj.bbox.x2, obj.bbox.y2}},
            {"track_id", obj.track_id}
        });
    }
    meta_json["objects"] = obj_arr;

    std::string meta_str = meta_json.dump();
    uint32_t meta_len = static_cast<uint32_t>(meta_str.size());

    QByteArray packet;
    packet.reserve(static_cast<int>(HEADER_LENGTH_V2 + meta_len + jpeg_data.size()));

    write_u32_be(packet, MAGIC_BYTES);
    write_u8(packet, PROTOCOL_VERSION);
    write_u8(packet, 1); // 1 = JPEG
    write_u8(packet, 0);
    write_u8(packet, HEADER_LENGTH_V2);
    static std::atomic<uint32_t> seq{0};
    write_u32_be(packet, seq.fetch_add(1));
    write_u32_be(packet, meta_len);
    write_u32_be(packet, static_cast<uint32_t>(jpeg_data.size()));

    packet.append(meta_str.c_str(), static_cast<int>(meta_len));
    packet.append(reinterpret_cast<const char*>(jpeg_data.data()), static_cast<int>(jpeg_data.size()));

    // forEachClient holds shared_lock and iterates — no vector allocation per frame.
    forEachClient(camera_id, [&](const ClientInfo& client) {
        sendBinaryDropIfBusy(client, packet);
    });
}

void CameraStreamManager::broadcastH264Frame(
    int camera_id,
    const QByteArray& nalu_data,
    const std::vector<inference::TrackedObject>& objects,
    uint64_t timestamp_us,
    bool is_keyframe,
    const std::string& codec,
    const std::string& decoder_config_b64,
    int frame_width,
    int frame_height
) {
    {
        std::shared_lock<std::shared_mutex> lock(camera_mutex_);
        if (camera_clients_.find(camera_id) == camera_clients_.end()) return;
    }

    nlohmann::json meta_json;
    meta_json["ts_us"] = timestamp_us;
    meta_json["keyframe"] = is_keyframe;
    meta_json["codec"] = codec;
    meta_json["width"] = frame_width > 0 ? frame_width : 640;
    meta_json["height"] = frame_height > 0 ? frame_height : 360;
    if (!decoder_config_b64.empty()) meta_json["decoder_config"] = decoder_config_b64;

    nlohmann::json obj_arr = nlohmann::json::array();
    for (const auto& obj : objects) {
        obj_arr.push_back({
            {"label", obj.label},
            {"confidence", obj.confidence},
            {"bbox", {obj.bbox.x1, obj.bbox.y1, obj.bbox.x2, obj.bbox.y2}},
            {"track_id", obj.track_id}
        });
    }
    meta_json["objects"] = obj_arr;

    std::string meta_str = meta_json.dump();
    uint32_t meta_len = static_cast<uint32_t>(meta_str.size());

    QByteArray packet;
    packet.reserve(static_cast<int>(HEADER_LENGTH_V2 + meta_len + nalu_data.size()));

    write_u32_be(packet, MAGIC_BYTES);
    write_u8(packet, PROTOCOL_VERSION);
    write_u8(packet, 2); // 2 = H264
    write_u8(packet, is_keyframe ? 0x01 : 0x00);
    write_u8(packet, HEADER_LENGTH_V2);
    static std::atomic<uint32_t> h264_seq{0};
    write_u32_be(packet, h264_seq.fetch_add(1));
    write_u32_be(packet, meta_len);
    write_u32_be(packet, static_cast<uint32_t>(nalu_data.size()));

    packet.append(meta_str.c_str(), static_cast<int>(meta_len));
    packet.append(nalu_data);

    forEachClient(camera_id, [&](const ClientInfo& client) {
        sendBinaryDropIfBusy(client, packet, is_keyframe);
    });
}

void CameraStreamManager::broadcastFmp4Fragment(
    int camera_id,
    const std::vector<unsigned char>& fragment,
    const nlohmann::json& metadata,
    bool is_init_segment
) {
    if (fragment.empty()) return;

    auto meta_json = metadata;
    meta_json["segment_type"] = is_init_segment ? "init" : "media";

    std::string meta_str = meta_json.dump();
    uint32_t meta_len = (uint32_t)meta_str.size();

    QByteArray packet;
    packet.reserve(HEADER_LENGTH_V2 + meta_len + (uint32_t)fragment.size());

    write_u32_be(packet, MAGIC_BYTES);
    write_u8(packet, PROTOCOL_VERSION);
    write_u8(packet, FRAME_TYPE_FMP4);
    write_u8(packet, is_init_segment ? 0x01 : 0x00);
    write_u8(packet, HEADER_LENGTH_V2);
    static std::atomic<uint32_t> fmp4_seq{0};
    write_u32_be(packet, fmp4_seq.fetch_add(1));
    write_u32_be(packet, meta_len);
    write_u32_be(packet, (uint32_t)fragment.size());

    packet.append(meta_str.c_str(), static_cast<int>(meta_len));
    packet.append(reinterpret_cast<const char*>(fragment.data()), static_cast<int>(fragment.size()));

    // Store init segment BEFORE broadcasting so any client that joins during
    // the send will receive the cached init packet from addClient().
    if (is_init_segment) {
        std::lock_guard<std::shared_mutex> lock(camera_mutex_);
        latest_fmp4_init_packets_[camera_id] = packet;
    }

    forEachClient(camera_id, [&](const ClientInfo& client) {
        sendBinaryDropIfBusy(client, packet, is_init_segment);
    });
}

void CameraStreamManager::broadcastEvent(int camera_id, const nlohmann::json& event) {
    QString msg = QString::fromStdString(event.dump());

    // Camera specific
    if (camera_id != 0) {
        for (const auto& client : snapshotClients(camera_id)) {
            sendText(client.socket, msg);
        }
    }

    // Global listeners
    for (const auto& client : snapshotClients(0)) {
        sendText(client.socket, msg);
    }
}

} // namespace streaming
} // namespace vms
