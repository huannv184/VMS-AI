#include "ipc/zmq_event_bridge.h"
#include "core/event_manager.h"
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

    // Reject obvious garbage. The ai-worker path lives in AiEventProcessor
    // (see processLicensePlate); this ZMQ entry-point is reserved for future
    // brand-camera-driven publishers (ANPR-capable hardware emitting via the
    // signed event bus). Apply the same drop policy so the gallery doesn't
    // accumulate "UNKNOWN" rows from misconfigured publishers.
    if (plate.plate_number.empty() || plate.plate_number == "UNKNOWN") {
        LOG_WARN("ZmqEventBridge LPR: dropped plate with empty/UNKNOWN text (camera={})",
                 plate.camera_id);
        return;
    }

    // BUG-ANPR-PIPELINE half-dead followup: pre-fix this handler built the
    // struct and discarded it — only LOG_INFO ran, the DB was never touched.
    // Now we actually persist + broadcast so any path that publishes "lpr"
    // envelopes lands in the same gallery as the in-process detector.
    try {
        vms::database::ANPRRepository repo;
        if (!repo.insertPlate(plate)) {
            LOG_ERROR("ZmqEventBridge LPR insert failed: camera={} plate='{}'",
                      plate.camera_id, plate.plate_number);
            return;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("ZmqEventBridge LPR insert exception: camera={} plate='{}' err={}",
                  plate.camera_id, plate.plate_number, e.what());
        return;
    }

    json broadcast_msg = {
        {"type", "alert"},
        {"event_type", "LPR"},
        {"camera_id", plate.camera_id},
        {"timestamp", plate.detected_at},
        {"severity", "low"},
        {"plate", plate.plate_number},
        {"confidence", plate.confidence},
        {"snapshot_url", plate.image_path}
    };
    try {
        auto& ws = streaming::CameraStreamManager::getInstance();
        if (plate.camera_id > 0) ws.broadcastEvent(plate.camera_id, broadcast_msg);
        ws.broadcastEvent(0, broadcast_msg);
    } catch (...) {
        // WS broadcast failures must never break the DB-insert success path.
    }

    LOG_INFO("ZmqEventBridge LPR persisted: camera={} plate='{}'",
             plate.camera_id, plate.plate_number);
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

// BUG-EVENT-DISPATCH-BYPASS-01 fix (2026-05-12): all five hardware-event
// handlers below previously called `DbManager::enqueueEvent` directly + did
// their own WS broadcast inline. That path writes the row to the events table
// but **completely skips** `EventManager::createEvent`, which is the gate
// where `RuleEngine::evaluateEvent` + `AlertManager::processEvent` get
// triggered. Net effect: operator-configured webhook/email/Telegram/alarm-relay
// rules NEVER fired for ZMQ-driven INTRUSION/LOITERING/FIRE/SMOKE/PPE events.
// EventsView showed the row because the DB insert succeeded; alerting was
// silently dead. Found during the AiEventProcessor + zmq_event_bridge race
// deep-dive on 2026-05-12.
//
// Fix: funnel every handler through `EventManager::createEvent`, which:
//   1. auto-generates a UUID id (same UUID v4 generator pattern as enqueueEvent),
//   2. calls `event_repo_->insertEvent` → batch writer → DB,
//   3. broadcasts the WS payload to per-camera + global channels,
//   4. fires `AlertManager::processEvent` (legacy alert_rules dispatch),
//   5. fires `RuleEngine::evaluateEvent` (modern rules+zones dispatch).
//
// The manual `streaming::CameraStreamManager::broadcastEvent(...)` blocks each
// handler used to do are now removed — `EventManager::createEvent` performs
// the same broadcast inline. Schema difference: createEvent additionally
// includes `id`/`confidence`/`bbox` extracted from metadata, which is strictly
// more information for the frontend (`useWebSocket.js` switches on
// `event_type` so extra fields are ignored).
//
// RuleEngine + AlertManager are both thread-safe (lock_guard on rules_mutex_),
// so fanning in from the ZMQ thread plus the AiEventProcessor worker pool is
// safe.
namespace {
// Helper: build a vms::Event from a ZMQ payload + dispatch through the canonical path.
void dispatchHardwareEvent(int camera_id,
                           std::time_t ts,
                           const std::string& event_type,
                           const std::string& description,
                           const std::string& severity,
                           const json& msg) {
    vms::Event evt;
    // id left empty — EventManager::createEvent auto-generates a UUID before
    // delegating to event_repo_->insertEvent. Pre-fix this was where BUG-DB-01
    // bit us when callers built deterministic ids; both paths now safe.
    evt.camera_id     = camera_id;
    evt.event_type    = event_type;
    evt.description   = description;
    evt.severity      = severity;
    evt.timestamp     = ts;
    evt.metadata_json = msg.dump();
    evt.snapshot_path = msg.value("snapshot", "");

    try {
        vms::core::EventManager::getInstance().createEvent(evt);
    } catch (const std::exception& e) {
        LOG_ERROR("ZmqEventBridge: createEvent failed for {} cam={}: {}",
                  event_type, camera_id, e.what());
    }
}
} // anonymous namespace

void ZmqEventBridge::handleIntrusion(const json& msg) {
    dispatchHardwareEvent(
        msg.value("camera_id", -1),
        (std::time_t)msg.value("timestamp", (long long)std::time(nullptr)),
        "INTRUSION",
        "Phát hiện đối tượng xâm nhập vùng cấm",
        "high",
        msg);
}

void ZmqEventBridge::handleLoitering(const json& msg) {
    dispatchHardwareEvent(
        msg.value("camera_id", -1),
        (std::time_t)msg.value("timestamp", (long long)std::time(nullptr)),
        "LOITERING",
        "Phát hiện đối tượng lảng vảng trong khu vực quá thời gian quy định",
        "medium",
        msg);
}

void ZmqEventBridge::handleFire(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    dispatchHardwareEvent(
        camera_id,
        (std::time_t)msg.value("timestamp", (long long)std::time(nullptr)),
        "FIRE",
        "Phát hiện ngọn lửa qua camera " + std::to_string(camera_id),
        "critical",
        msg);
}

void ZmqEventBridge::handleSmoke(const json& msg) {
    int camera_id = msg.value("camera_id", -1);
    dispatchHardwareEvent(
        camera_id,
        (std::time_t)msg.value("timestamp", (long long)std::time(nullptr)),
        "SMOKE",
        "Phát hiện khói bất thường qua camera " + std::to_string(camera_id),
        "high",
        msg);
}

void ZmqEventBridge::handlePPEViolation(const json& msg) {
    dispatchHardwareEvent(
        msg.value("camera_id", -1),
        (std::time_t)msg.value("timestamp", (long long)std::time(nullptr)),
        "PPE_VIOLATION",
        "Phát hiện nhân viên không đội mũ bảo hiểm hoặc mặc áo phản quang",
        "warning",
        msg);
}

} // namespace ipc
} // namespace vms
