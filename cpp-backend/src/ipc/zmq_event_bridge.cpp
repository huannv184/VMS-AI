#include "ipc/zmq_event_bridge.h"
#include "database/anpr_repository.h"
#include "database/traffic_repository.h"
#include "database/event_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"
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
    json msg;
    try {
        msg = json::parse(raw_json);
    } catch (const json::exception& e) {
        LOG_WARN("ZmqEventBridge: invalid JSON received: {} – msg: {}", e.what(), raw_json.substr(0, 120));
        return;
    }

    if (!msg.contains("type")) {
        LOG_WARN("ZmqEventBridge: message missing 'type' field");
        return;
    }

    std::string type = msg.value("type", "");
    LOG_DEBUG("ZmqEventBridge dispatching event type='{}'", type);

    // D3: Emit Qt Signal for Phase C / UI consumers
    Q_EMIT eventReceived(QString::fromStdString(type), QString::fromStdString(raw_json));

    try {
        if      (type == "lpr")           handleLpr(msg);
        else if (type == "traffic_count") handleTrafficCount(msg);
        else if (type == "intrusion")     handleIntrusion(msg);
        else if (type == "loitering")     handleLoitering(msg);
        else if (type == "fire")          handleFire(msg);
        else if (type == "smoke")         handleSmoke(msg);
        else if (type == "ppe_violation") handlePPEViolation(msg);
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
    plate.image_path   = msg.value("image_path",   std::string(""));
    plate.detected_at  = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));

    // Validate plate length
    if (plate.plate_number.empty() || plate.plate_number.length() > 20) {
        LOG_WARN("ZmqEventBridge LPR: invalid plate number '{}'", plate.plate_number);
        return;
    }

    database::ANPRRepository anpr_repo;
    if (!anpr_repo.insertPlate(plate)) {
        LOG_ERROR("ZmqEventBridge LPR: failed to insert plate '{}'", plate.plate_number);
        return;
    }

    // Also create an event for the event feed
    Event evt;
    evt.id         = "lpr-" + std::to_string(plate.detected_at) + "-" + plate.plate_number;
    evt.camera_id  = plate.camera_id;
    evt.event_type = "lpr";
    evt.description = "Phát hiện biển số: " + plate.plate_number + " (" + plate.vehicle_type + ")";
    evt.timestamp  = plate.detected_at;

    json meta;
    meta["plate"]        = plate.plate_number;
    meta["vehicle_type"] = plate.vehicle_type;
    meta["confidence"]   = plate.confidence;
    evt.metadata_json = meta.dump();

    database::EventRepository ev_repo;
    ev_repo.insertEvent(evt);

    LOG_INFO("ZmqEventBridge LPR: plate='{}' cam={} conf={:.2f} saved",
             plate.plate_number, plate.camera_id, plate.confidence);
}

void ZmqEventBridge::handleTrafficCount(const json& msg) {
    if (!msg.contains("camera_id") || !msg.contains("count") ||
        !msg.contains("period_start") || !msg.contains("period_end")) {
        LOG_WARN("ZmqEventBridge traffic_count: missing required fields");
        return;
    }

    TrafficCount tc;
    tc.camera_id    = msg.value("camera_id",    -1);
    tc.roi_id       = msg.value("roi_id",       -1);
    tc.direction    = msg.value("direction",    std::string("both"));
    tc.vehicle_type = msg.value("vehicle_type", std::string("all"));
    tc.count        = msg.value("count",        0);
    tc.period_start = (std::time_t)msg.value("period_start", (long long)0);
    tc.period_end   = (std::time_t)msg.value("period_end",   (long long)0);

    database::TrafficRepository repo;
    if (repo.insertCount(tc)) {
        LOG_DEBUG("ZmqEventBridge traffic_count: cam={} dir={} type={} count={}",
                  tc.camera_id, tc.direction, tc.vehicle_type, tc.count);
    } else {
        LOG_ERROR("ZmqEventBridge traffic_count: failed to insert count");
    }
}

void ZmqEventBridge::handleIntrusion(const json& msg) {
    if (!msg.contains("camera_id")) {
        LOG_WARN("ZmqEventBridge intrusion: missing camera_id");
        return;
    }

    int camera_id = msg.value("camera_id", -1);
    int roi_id    = msg.value("roi_id",    -1);
    int track_id  = msg.value("track_id",  -1);
    std::string severity = msg.value("severity", "high");
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));

    Event evt;
    evt.id = "intrusion-" + std::to_string(ts) + "-cam" + std::to_string(camera_id)
           + "-trk" + std::to_string(track_id);
    evt.camera_id  = camera_id;
    evt.event_type = "intrusion";
    evt.description = "Phát hiện xâm nhập vùng cấm (ROI " + std::to_string(roi_id) + ")";
    evt.timestamp  = ts;

    json meta;
    meta["roi_id"]   = roi_id;
    meta["track_id"] = track_id;
    meta["severity"] = severity;
    evt.metadata_json = meta.dump();

    database::EventRepository ev_repo;
    ev_repo.insertEvent(evt);

    LOG_WARN("ZmqEventBridge INTRUSION: cam={} roi={} trk={} severity={}",
             camera_id, roi_id, track_id, severity);
}

void ZmqEventBridge::handleLoitering(const json& msg) {
    if (!msg.contains("camera_id")) {
        LOG_WARN("ZmqEventBridge loitering: missing camera_id");
        return;
    }

    int camera_id     = msg.value("camera_id",    -1);
    int roi_id        = msg.value("roi_id",       -1);
    int track_id      = msg.value("track_id",     -1);
    int duration_sec  = msg.value("duration_sec", 0);
    std::time_t ts    = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));

    Event evt;
    evt.id = "loitering-" + std::to_string(ts) + "-cam" + std::to_string(camera_id)
           + "-trk" + std::to_string(track_id);
    evt.camera_id  = camera_id;
    evt.event_type = "loitering";
    evt.description = "Đối tượng lảng vảng " + std::to_string(duration_sec) + "s trong ROI " + std::to_string(roi_id);
    evt.timestamp  = ts;

    json meta;
    meta["roi_id"]      = roi_id;
    meta["track_id"]    = track_id;
    meta["duration_sec"]= duration_sec;
    evt.metadata_json = meta.dump();

    database::EventRepository ev_repo;
    ev_repo.insertEvent(evt);

    LOG_WARN("ZmqEventBridge LOITERING: cam={} roi={} trk={} {}s",
             camera_id, roi_id, track_id, duration_sec);
}

void ZmqEventBridge::handleFire(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));
    
    Event evt;
    evt.id = "fire-" + std::to_string(ts) + "-cam" + std::to_string(camera_id);
    evt.camera_id  = camera_id;
    evt.event_type = "fire";
    evt.description = "🔥 PHÁT HIỆN LỬA / CHÁY NỔ!";
    evt.timestamp  = ts;

    json meta;
    meta["severity"] = "critical";
    meta["is_emergency"] = true;
    if (msg.contains("confidence")) meta["confidence"] = msg["confidence"];
    evt.metadata_json = meta.dump();

    database::EventRepository ev_repo;
    ev_repo.insertEvent(evt);

    LOG_CRITICAL("ZmqEventBridge FIRE DETECTED on camera {}", camera_id);

    // Broadcast emergency
    json broadcast_msg;
    broadcast_msg["type"] = "event";
    broadcast_msg["event"] = {
        {"id", evt.id},
        {"camera_id", evt.camera_id},
        {"event_type", evt.event_type},
        {"description", evt.description},
        {"timestamp", evt.timestamp},
        {"severity", "critical"},
        {"is_emergency", true}
    };
    streaming::CameraStreamManager::getInstance().broadcastEvent(0, broadcast_msg);
    if (camera_id > 0) streaming::CameraStreamManager::getInstance().broadcastEvent(camera_id, broadcast_msg);
}

void ZmqEventBridge::handleSmoke(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));
    
    Event evt;
    evt.id = "smoke-" + std::to_string(ts) + "-cam" + std::to_string(camera_id);
    evt.camera_id  = camera_id;
    evt.event_type = "smoke";
    evt.description = "💨 PHÁT HIỆN KHÓI MẬT ĐỘ CAO!";
    evt.timestamp  = ts;

    json meta;
    meta["severity"] = "high";
    meta["is_emergency"] = true;
    evt.metadata_json = meta.dump();

    database::EventRepository ev_repo;
    ev_repo.insertEvent(evt);

    LOG_ERROR("ZmqEventBridge SMOKE DETECTED on camera {}", camera_id);

    // Broadcast emergency
    json broadcast_msg;
    broadcast_msg["type"] = "event";
    broadcast_msg["event"] = {
        {"id", evt.id},
        {"camera_id", evt.camera_id},
        {"event_type", evt.event_type},
        {"description", evt.description},
        {"timestamp", evt.timestamp},
        {"severity", "high"},
        {"is_emergency", true}
    };
    streaming::CameraStreamManager::getInstance().broadcastEvent(0, broadcast_msg);
    if (camera_id > 0) streaming::CameraStreamManager::getInstance().broadcastEvent(camera_id, broadcast_msg);
}

void ZmqEventBridge::handlePPEViolation(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    std::time_t ts = (std::time_t)msg.value("timestamp", (long long)std::time(nullptr));
    std::string violation = msg.value("violation", "unknown"); // hardhat, vest
    
    Event evt;
    evt.id = "ppe-" + std::to_string(ts) + "-cam" + std::to_string(camera_id) + "-" + violation;
    evt.camera_id  = camera_id;
    evt.event_type = "ppe_violation";
    evt.description = "Vi phạm an toàn LĐ: Không có " + violation;
    evt.timestamp  = ts;

    json meta;
    meta["violation"] = violation;
    meta["severity"] = "warning";
    evt.metadata_json = meta.dump();

    database::EventRepository ev_repo;
    ev_repo.insertEvent(evt);

    LOG_WARN("ZmqEventBridge PPE VIOLATION ({}): cam={}", violation, camera_id);

    json broadcast_msg;
    broadcast_msg["type"] = "event";
    broadcast_msg["event"] = {
        {"id", evt.id},
        {"camera_id", evt.camera_id},
        {"event_type", evt.event_type},
        {"description", evt.description},
        {"timestamp", evt.timestamp},
        {"severity", "warning"}
    };
    if (camera_id > 0) streaming::CameraStreamManager::getInstance().broadcastEvent(camera_id, broadcast_msg);
}

} // namespace ipc
} // namespace vms
