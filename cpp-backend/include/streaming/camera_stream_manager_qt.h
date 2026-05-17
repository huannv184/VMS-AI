#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <atomic>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>

#include <QObject>
#include <QPointer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QByteArray>

#include "inference/tracking.h"

namespace vms {
namespace streaming {

/**
 * CameraStreamManager (Qt-Powered Version)
 * 
 * Replaces Crow WebSocket with QWebSocketServer.
 * Uses Qt Parent-Child model and signal/slot mechanism for thread safety.
 */
class CameraStreamManager : public QObject {
    Q_OBJECT

public:
    static CameraStreamManager& getInstance();

    /**
     * Start the QWebSocketServer listening on the configured port.
     */
    void start();

    /**
     * Stop the server and close all connections.
     */
    void stop();

    void stopAll() { stop(); }

    /**
     * Subscribe a connection to a camera's stream.
     */
    void addClient(int camera_id, const QPointer<QWebSocket>& client);

    /**
     * Unsubscribe a connection from a camera's stream.
     */
    void removeClient(int camera_id, const QPointer<QWebSocket>& client);

    /**
     * Unsubscribe a connection from all cameras.
     */
    void removeClient(const QPointer<QWebSocket>& client);
    void removeClient(QWebSocket* client);

    bool isStreaming(int camera_id);
    bool startStream(int camera_id, const std::string& rtsp_url);
    size_t getClientCount(int camera_id);

    // Returns the actual bound port (may differ from config if port was in use)
    int getBoundPort() const { return bound_port_.load(); }

    // Broadcast methods
    void broadcastRawFrame(
        int camera_id,
        const std::vector<unsigned char>& jpeg_data,
        const std::vector<inference::TrackedObject>& objects,
        int frame_width = 640,
        int frame_height = 360
    );

    void broadcastH264Frame(
        int camera_id,
        const QByteArray& nalu_data,
        const std::vector<inference::TrackedObject>& objects,
        uint64_t timestamp_us = 0,
        bool is_keyframe = false,
        const std::string& codec = "avc1.42E01E",
        const std::string& decoder_config_b64 = "",
        int frame_width = 640,
        int frame_height = 360
    );

    void broadcastFmp4Fragment(
        int camera_id,
        const std::vector<unsigned char>& fragment,
        const nlohmann::json& metadata,
        bool is_init_segment = false
    );

    void broadcastEvent(int camera_id, const nlohmann::json& event);

    // 2026-05-17 WS observability trifecta (BUG-WS-NO-METRICS-01 +
    // BUG-WS-NO-CONN-CAP-01 + BUG-WS-NO-MSGSIZE-CAP-01). Wait-free atomic
    // snapshot of connection counters + cap-rejection counter; surfaced
    // via `GET /api/rules/stats` `websocket` block.
    struct ConnectionStats {
        std::uint64_t connections_total{0};       // monotonic — every accepted socket
        std::uint64_t connections_current{0};     // gauge — live sockets
        std::uint64_t peak_connections{0};        // monotonic max of connections_current
        std::uint64_t authed_total{0};            // monotonic — successful AUTH
        std::uint64_t auth_failed_total{0};       // monotonic — AUTH_ERR responses
        std::uint64_t ticket_replay_total{0};     // monotonic — JTI replay rejections
        std::uint64_t preauth_timeout_total{0};   // monotonic — 10s timer fired with no AUTH
        std::uint64_t conn_cap_rejected_total{0}; // monotonic — accept-time cap rejection
        std::uint64_t disconnects_total{0};       // monotonic — socketDisconnected fires
        std::size_t   distinct_ips{0};            // gauge — entries in per-IP map (read-only snapshot)
        int           max_connections_global{0};  // configured cap (0 = unlimited)
        int           max_connections_per_ip{0};  // configured cap (0 = unlimited)
    };
    ConnectionStats connectionStats() const;

public Q_SLOTS:
    void handleAiEvent(const QString& type, const QString& payload);

private Q_SLOTS:
    void onNewConnection();
    void processTextMessage(const QString& message);
    void processBinaryMessage(const QByteArray& message);
    void socketDisconnected();

private:
    CameraStreamManager();
    ~CameraStreamManager();
    CameraStreamManager(const CameraStreamManager&) = delete;
    CameraStreamManager& operator=(const CameraStreamManager&) = delete;

private:
    QPointer<QWebSocketServer> server_;
    
    struct ClientInfo {
        QPointer<QWebSocket> socket;
        std::shared_ptr<std::atomic<int>> inflight_counter;
    };

    // Per-camera subscriber sets.
    // Protected by camera_mutex_ for cross-thread access during broadcast.
    std::shared_mutex camera_mutex_;
    std::unordered_map<int, std::vector<ClientInfo>> camera_clients_;

    // Reverse map: socket ptr → camera_id.
    // Turns O(cameras × clients) disconnect scan into O(1) lookup.
    // Protected by camera_mutex_.
    std::unordered_map<QWebSocket*, int> socket_to_camera_;

    // Cache for last init segment per camera to replay for new clients
    std::unordered_map<int, QByteArray> latest_fmp4_init_packets_;

    // Helpers
    bool sendBinary(const QPointer<QWebSocket>& client, const QByteArray& data);
    bool sendBinaryDropIfBusy(const ClientInfo& client, const QByteArray& data, bool is_mandatory = false);
    bool sendText(const QPointer<QWebSocket>& client, const QString& msg);

    // Iterate subscribers under shared_lock — avoids per-broadcast vector allocation.
    // Fn must not acquire camera_mutex_ (deadlock) but may call QMetaObject::invokeMethod.
    template<typename Fn>
    void forEachClient(int camera_id, Fn&& fn) {
        std::shared_lock<std::shared_mutex> lock(camera_mutex_);
        auto it = camera_clients_.find(camera_id);
        if (it == camera_clients_.end()) return;
        for (const auto& client : it->second) {
            if (client.socket) fn(client);
        }
    }

    std::vector<ClientInfo> snapshotClients(int camera_id);

    // Actual bound port — set after listen() succeeds; -1 if server not started
    std::atomic<int> bound_port_{-1};

    // Backpressure metrics — lock-free, O(1)
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> frames_sent_{0};

    // Slow consumer threshold (bytes in-flight per client).
    // 256 KB: at 4 Mbps stream, this is ~500ms buffer — enough for jitter without
    // allowing a slow client to consume GBs of RAM across hundreds of cameras.
    static constexpr int kBackpressureThreshold = 256 * 1024; // 256 KB (was 2 MB)
};

} // namespace streaming
} // namespace vms
