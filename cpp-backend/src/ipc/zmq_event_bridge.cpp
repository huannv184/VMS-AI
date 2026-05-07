#include "ipc/zmq_event_bridge.h"
#include "database/anpr_repository.h"
#include "database/traffic_repository.h"
#include "database/traffic_repository.h"
#include "database/event_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include "utils/config.h"
#include "utils/sha256.h"
#include "streaming/camera_stream_manager_qt.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <random>
#include <QThread>

using json = nlohmann::json;

namespace vms {
namespace ipc {

namespace {

// Constant-time string comparison to prevent timing attacks
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

}

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
ZmqEventBridge& ZmqEventBridge::getInstance() {
    static ZmqEventBridge instance;
    return instance;
}

ZmqEventBridge::~ZmqEventBridge() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void ZmqEventBridge::start() {
    if (running_.load()) {
        LOG_WARN("ZmqEventBridge already running");
        return;
    }

    // Read configuration from settings DB
    auto& db = vms::database::DbManager::getInstance();
    std::string enabled_str = db.getSetting("zmq_bridge.enabled", "true");
    if (enabled_str != "true") {
        LOG_INFO("ZmqEventBridge disabled by setting zmq_bridge.enabled=false");
        return;
    }

    std::string endpoint = db.getSetting("zmq_bridge.endpoint", "tcp://127.0.0.1:5555");
    LOG_INFO("ZmqEventBridge starting – subscribing to {}", endpoint);

    stop_requested_.store(false);
    running_.store(true);

    thread_ = QThread::create([this, endpoint]() {
        run(endpoint);
    });
    thread_->start();

    LOG_INFO("ZmqEventBridge thread launched (ZMQ SUB on {})", endpoint);
}

void ZmqEventBridge::stop() {
    if (!running_.load()) return;

    LOG_INFO("ZmqEventBridge stopping ...");
    stop_requested_.store(true);

    // Close socket so recv unblocks immediately
    if (socket_) {
        try { socket_->close(); } catch (...) {}
        socket_.reset();
    }
    if (context_) {
        try { context_->close(); } catch (...) {}
        context_.reset();
    }

    if (thread_) {
        thread_->wait();
        delete thread_;
        thread_ = nullptr;
    }
    running_.store(false);
    LOG_INFO("ZmqEventBridge stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
// Subscriber thread
// ─────────────────────────────────────────────────────────────────────────────
void ZmqEventBridge::run(const std::string& endpoint) {
    // We create context/socket inside the thread so they are owned by it.
    // On stop() the socket is closed externally to unblock recv.
    try {
        context_ = std::make_unique<zmq::context_t>(1);
        socket_  = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::sub);

        // Subscribe to ALL topics (empty filter)
        socket_->set(zmq::sockopt::subscribe, "");

        // 500 ms receive timeout → retry without CPU spin
        socket_->set(zmq::sockopt::rcvtimeo, 500);

        // Linger 0: don't wait on unsent messages during close
        socket_->set(zmq::sockopt::linger, 0);

        // Non-blocking reconnect if AI Worker is not yet online
        socket_->set(zmq::sockopt::reconnect_ivl, 2000); // 2 s between retries

        socket_->connect(endpoint);
        LOG_INFO("ZmqEventBridge connected to {}", endpoint);
    } catch (const zmq::error_t& e) {
        LOG_ERROR("ZmqEventBridge failed to create socket: {}", e.what());
        running_.store(false);
        return;
    }

    // ── Main receive loop ─────────────────────────────────────────────────────
    while (!stop_requested_.load()) {
        try {
            zmq::message_t msg;
            auto result = socket_->recv(msg, zmq::recv_flags::none);

            if (!result.has_value()) {
                // EAGAIN / timeout — AI Worker offline or no message, just retry
                continue;
            }

            std::string raw(static_cast<char*>(msg.data()), msg.size());
            if (raw.empty()) continue;

            dispatchMessage(raw);

        } catch (const zmq::error_t& e) {
            if (stop_requested_.load()) break; // Expected on shutdown
            if (e.num() == ETERM)              break; // Context terminated
            LOG_WARN("ZmqEventBridge recv error: {} – retrying", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LOG_INFO("ZmqEventBridge receive loop exited");
    running_.store(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch
// ─────────────────────────────────────────────────────────────────────────────
void ZmqEventBridge::dispatchMessage(const std::string& raw_json) {
    messages_received_.fetch_add(1, std::memory_order_relaxed);
    json msg;
    try {
        msg = json::parse(raw_json);
    } catch (const json::exception& e) {
        messages_dropped_.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("ZmqEventBridge: invalid JSON received: {} – msg: {}", e.what(), raw_json.substr(0, 120));
        return;
    }

    // [MUST-FIX] ZMQ Security: Validate HMAC signature
    // Prevents attackers on the local network from spoofing AI events.
    // The message MUST be an envelope: { "payload": {...}, "signature": "hex" }
    if (!msg.contains("signature") || !msg.contains("payload")) {
        messages_dropped_.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("[ZmqBridge] Rejected message: Missing signature or payload envelope");
        return;
    }

    std::string payload_str = msg["payload"].dump();
    std::string provided_sig = msg["signature"].get<std::string>();
    
    const auto& auth_cfg = vms::Config::getInstance().getAuthConfig();
    // signature = SHA256(payload_json_string + secret_key)
    std::string expected_sig = vms::utils::SHA256::hash(payload_str + auth_cfg.secret_key);

    if (!constantTimeEquals(provided_sig, expected_sig)) {
        messages_dropped_.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("[ZmqBridge] Rejected message: Invalid HMAC signature. Potential spoofing attempt!");
        return;
    }

    // Use the inner payload for processing
    json payload = msg["payload"];

    if (!payload.contains("type")) {
        LOG_WARN("ZmqEventBridge: payload missing 'type' field");
        return;
    }

    std::string type = payload.value("type", "");
    LOG_DEBUG("ZmqEventBridge dispatching event type='{}'", type);

    // D3: Emit Qt Signal for Phase C / UI consumers
    Q_EMIT eventReceived(QString::fromStdString(type), QString::fromStdString(payload.dump()));

    try {
        if      (type == "lpr")           handleLpr(payload);
        else if (type == "traffic_count") handleTrafficCount(payload);
        else if (type == "intrusion")     handleIntrusion(payload);
        else if (type == "loitering")     handleLoitering(payload);
        else if (type == "fire")          handleFire(payload);
        else if (type == "smoke")         handleSmoke(payload);
        else if (type == "ppe_violation") handlePPEViolation(payload);
        else {
            LOG_DEBUG("ZmqEventBridge: unknown event type '{}' – ignored", type);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("ZmqEventBridge: error handling event type='{}': {}", type, e.what());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Handlers
// ─────────────────────────────────────────────────────────────────────────────
void ZmqEventBridge::handleLpr(const json& msg) {
    if (!msg.contains("plate") || !msg.contains("camera_id")) {
        LOG_WARN("ZmqEventBridge LPR: missing required fields (plate, camera_id)");
        return;
    }

    LicensePlate plate;
    plate.plate_number = msg.value("plate",        std::string("UNKNOWN"));
    plate.vehicle_type = msg.value("vehicle_type", std::string("unknown"));
    plate.color        = msg.value("color",        std::string(""));
    plate.camera_id    = msg.value("camera_id",    -1);
    plate.confidence   = msg.value("confidence",   0.0f);
    plate.detected_at  = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));
    plate.image_path   = msg.value("image_path",   std::string(""));

    // Async DB write
    try {
        LOG_INFO("LPR event received: {} - {}", plate.plate_number, plate.camera_id);
    } catch (...) {}
}

void ZmqEventBridge::handleTrafficCount(const json& msg) {
    TrafficCount count;
    count.camera_id    = msg.value("camera_id",    -1);
    count.roi_id       = msg.value("roi_id",       -1);
    count.direction    = msg.value("direction",    std::string("both"));
    count.vehicle_type = msg.value("vehicle_type", std::string("all"));
    count.count        = msg.value("count",        0);
    count.period_start = (std::time_t)msg.value("start_time", (long long)std::time(nullptr));
    count.period_end   = (std::time_t)msg.value("end_time",   (long long)std::time(nullptr));

    try {
        vms::database::TrafficRepository traffic_repo;
        traffic_repo.insertCount(count);
    } catch (...) {}
}

void ZmqEventBridge::handleIntrusion(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));

    // id intentionally left empty — DbManager::enqueueEvent auto-generates a
    // unique UUID. Building "<type>-<sec>-cam<id>" caused INSERT OR IGNORE
    // silent drops when two events of the same type fired on the same camera
    // within the same second (BUG-DB-01 pattern, see past-bugs.md).
    Event evt;
    evt.camera_id  = camera_id;
    evt.event_type = "INTRUSION";
    evt.description = "Phát hiện đối tượng xâm nhập vùng cấm";
    evt.severity = "high";
    evt.timestamp = ts;
    evt.metadata_json = msg.dump();
    evt.snapshot_path = msg.value("snapshot", "");

    try {
        vms::database::DbManager::getInstance().enqueueEvent(evt);
    } catch (...) {}
    
    // Broadcast for UI alerts
    // Format must match event_manager.cpp WS broadcast schema so frontend
    // useWebSocket.js handler 'alert' case fires (it doesn't switch on per-event names).
    json broadcast_msg = {
        {"type", "alert"},
        {"event_type", evt.event_type},
        {"camera_id", camera_id},
        {"timestamp", evt.timestamp},
        {"severity", evt.severity},
        {"description", evt.description},
        {"snapshot_url", evt.snapshot_path}
    };
    auto& ws = streaming::CameraStreamManager::getInstance();
    if (camera_id > 0) ws.broadcastEvent(camera_id, broadcast_msg);
    ws.broadcastEvent(0, broadcast_msg); // global channel — dashboard subscribers
}

void ZmqEventBridge::handleLoitering(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));

    Event evt;
    evt.camera_id  = camera_id;
    evt.event_type = "LOITERING";
    evt.description = "Phát hiện đối tượng lảng vảng trong khu vực quá thời gian quy định";
    evt.severity = "medium";
    evt.timestamp = ts;
    evt.metadata_json = msg.dump();
    evt.snapshot_path = msg.value("snapshot", "");

    try {
        vms::database::DbManager::getInstance().enqueueEvent(evt);
    } catch (...) {}

    json broadcast_msg = {
        {"type", "alert"},
        {"event_type", evt.event_type},
        {"camera_id", camera_id},
        {"timestamp", evt.timestamp},
        {"severity", evt.severity},
        {"description", evt.description},
        {"snapshot_url", evt.snapshot_path}
    };
    auto& ws = streaming::CameraStreamManager::getInstance();
    if (camera_id > 0) ws.broadcastEvent(camera_id, broadcast_msg);
    ws.broadcastEvent(0, broadcast_msg); // global channel — dashboard subscribers
}

void ZmqEventBridge::handleFire(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));

    Event evt;
    evt.camera_id  = camera_id;
    evt.event_type = "FIRE";
    evt.description = "Phát hiện ngọn lửa qua camera " + std::to_string(camera_id);
    evt.severity = "critical";
    evt.timestamp = ts;
    evt.metadata_json = msg.dump();
    evt.snapshot_path = msg.value("snapshot", "");

    try {
        vms::database::DbManager::getInstance().enqueueEvent(evt);
    } catch (...) {}

    json broadcast_msg = {
        {"type", "alert"},
        {"event_type", evt.event_type},
        {"camera_id", camera_id},
        {"timestamp", evt.timestamp},
        {"severity", evt.severity},
        {"description", evt.description},
        {"snapshot_url", evt.snapshot_path}
    };
    auto& ws = streaming::CameraStreamManager::getInstance();
    if (camera_id > 0) ws.broadcastEvent(camera_id, broadcast_msg);
    ws.broadcastEvent(0, broadcast_msg); // global channel — dashboard subscribers
}

void ZmqEventBridge::handleSmoke(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));

    Event evt;
    evt.camera_id  = camera_id;
    evt.event_type = "SMOKE";
    evt.description = "Phát hiện khói bất thường qua camera " + std::to_string(camera_id);
    evt.severity = "high";
    evt.timestamp = ts;
    evt.metadata_json = msg.dump();
    evt.snapshot_path = msg.value("snapshot", "");

    try {
        vms::database::DbManager::getInstance().enqueueEvent(evt);
    } catch (...) {}

    json broadcast_msg = {
        {"type", "alert"},
        {"event_type", evt.event_type},
        {"camera_id", camera_id},
        {"timestamp", evt.timestamp},
        {"severity", evt.severity},
        {"description", evt.description},
        {"snapshot_url", evt.snapshot_path}
    };
    auto& ws = streaming::CameraStreamManager::getInstance();
    if (camera_id > 0) ws.broadcastEvent(camera_id, broadcast_msg);
    ws.broadcastEvent(0, broadcast_msg); // global channel — dashboard subscribers
}

void ZmqEventBridge::handlePPEViolation(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));

    Event evt;
    evt.camera_id  = camera_id;
    evt.event_type = "PPE_VIOLATION";
    evt.description = "Phát hiện nhân viên không đội mũ bảo hiểm hoặc mặc áo phản quang";
    evt.severity = "warning";
    evt.timestamp = ts;
    evt.metadata_json = msg.dump();
    evt.snapshot_path = msg.value("snapshot", "");

    try {
        vms::database::DbManager::getInstance().enqueueEvent(evt);
    } catch (...) {}

    json broadcast_msg = {
        {"type", "alert"},
        {"event_type", evt.event_type},
        {"camera_id", camera_id},
        {"timestamp", evt.timestamp},
        {"severity", evt.severity},
        {"description", evt.description},
        {"snapshot_url", evt.snapshot_path}
    };
    auto& ws = streaming::CameraStreamManager::getInstance();
    if (camera_id > 0) ws.broadcastEvent(camera_id, broadcast_msg);
    ws.broadcastEvent(0, broadcast_msg); // global channel — dashboard subscribers
}

} // namespace ipc
} // namespace vms
