#include "core/camera_event_service.h"
#include "streaming/camera_stream_manager_qt.h"
#include "core/brands/core_factory.hpp"
#include "core/event_manager.h"
#include "database/camera_repository.h"
#include "database/db_manager.h"
#include "database/models.h"
#include "database/traffic_repository.h"
#include "utils/logger.h"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace vms {
namespace core {

namespace {

// Severity rule of thumb: detection events (intrusion / line_crossing /
// loitering / fire / smoke) escalate to "high" because they typically demand
// human acknowledgement; motion / face / tampering / audio default to
// "medium"; everything else stays "low". Mirrors the severity ladder used by
// ZmqEventBridge so the AlertManager/RuleEngine dispatch table doesn't have to
// special-case hardware-driven events differently from AI-driven ones.
const char* severityForEventType(const std::string& evt) {
    if (evt == "intrusion" || evt == "line_crossing" || evt == "loitering" ||
        evt == "fire"      || evt == "smoke") {
        return "high";
    }
    if (evt == "motion_detect" || evt == "face" || evt == "tampering" ||
        evt == "audio_alarm") {
        return "medium";
    }
    return "low";
}

// Brand-agnostic event_type → human-readable Vietnamese description so the
// EventsView shows a useful row instead of just the raw token.
std::string descriptionForEventType(const std::string& evt, const std::string& brand) {
    const std::string brand_tag = brand.empty() ? "Phần cứng" : brand;
    if (evt == "motion_detect")  return "[" + brand_tag + "] Phát hiện chuyển động";
    if (evt == "line_crossing")  return "[" + brand_tag + "] Phát hiện đối tượng vượt qua đường ranh";
    if (evt == "intrusion")      return "[" + brand_tag + "] Phát hiện đối tượng xâm nhập vùng";
    if (evt == "loitering")      return "[" + brand_tag + "] Phát hiện đối tượng lảng vảng";
    if (evt == "tampering")      return "[" + brand_tag + "] Cảnh báo camera bị che/đổi cảnh";
    if (evt == "audio_alarm")    return "[" + brand_tag + "] Cảnh báo âm thanh bất thường";
    if (evt == "face")           return "[" + brand_tag + "] Phát hiện khuôn mặt";
    if (evt == "fire")           return "[" + brand_tag + "] Cảnh báo cháy";
    if (evt == "smoke")          return "[" + brand_tag + "] Cảnh báo khói";
    if (evt == "hardware_alarm") return "[" + brand_tag + "] Tín hiệu báo động phần cứng";
    return "[" + brand_tag + "] " + evt;
}

} // namespace

CameraEventService& CameraEventService::getInstance() {
    static CameraEventService instance;
    return instance;
}

CameraEventService::~CameraEventService() {
    stopAll();
}

void CameraEventService::startListening(const std::string& cam_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(cam_id) != sessions_.end()) return;

    auto session = std::make_shared<EventSession>();
    session->cam_id = cam_id;
    session->started_at = (long long)std::time(nullptr);
    session->state = static_cast<int>(SubscriptionState::Pending);
    session->worker = std::thread(&CameraEventService::workerLoop, this, session);
    sessions_[cam_id] = session;
    LOG_INFO("CameraEventService started listening for camera {}", cam_id);
}

const char* CameraEventService::subscriptionStateName(SubscriptionState s) {
    switch (s) {
        case SubscriptionState::Pending: return "pending";
        case SubscriptionState::Active:  return "active";
        case SubscriptionState::Failed:  return "failed";
        case SubscriptionState::Stopped: return "stopped";
    }
    return "unknown";
}

nlohmann::json CameraEventService::getStatus(const std::string& cam_id) {
    std::shared_ptr<EventSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(cam_id);
        if (it == sessions_.end()) {
            return nlohmann::json{
                {"cam_id",      cam_id},
                {"listening",   false},
                {"state",       "not_started"},
                {"reason",      "startListening has not been called for this camera"}
            };
        }
        session = it->second;
    }

    SubscriptionState st = static_cast<SubscriptionState>(session->state.load());
    long long now_ts = (long long)std::time(nullptr);
    long long last_event = session->last_event_at.load();

    return nlohmann::json{
        {"cam_id",            cam_id},
        {"listening",         true},
        {"state",             subscriptionStateName(st)},
        {"brand",             session->brand},
        {"started_at",        session->started_at.load()},
        {"last_attempt_at",   session->last_attempt_at.load()},
        {"last_event_at",     last_event},
        {"seconds_since_event", last_event > 0 ? (now_ts - last_event) : -1},
        {"total_events",      session->total_events.load()},
        {"reconnect_count",   session->reconnect_count.load()}
    };
}

void CameraEventService::stopListening(const std::string& cam_id) {
    std::shared_ptr<EventSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(cam_id);
        if (it == sessions_.end()) return;
        session = it->second;
        session->stop_flag = true;
        sessions_.erase(it);
    }
    // Join OUTSIDE the lock to prevent deadlock.
    if (session->worker.joinable()) {
        session->worker.join();
    }
    LOG_INFO("CameraEventService stopped listening for camera {}", cam_id);
}

void CameraEventService::stopAll() {
    std::vector<std::shared_ptr<EventSession>> all_sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : sessions_) {
            pair.second->stop_flag = true;
            all_sessions.push_back(pair.second);
        }
        sessions_.clear();
    }

    for (auto& session : all_sessions) {
        if (session->worker.joinable()) {
            session->worker.join();
        }
    }
}

CameraDiscovery::DiscoveryConfig CameraEventService::getCameraConfig(const std::string& cam_id) {
    CameraDiscovery::DiscoveryConfig cfg;
    try {
        int id = std::stoi(cam_id);
        
        database::CameraRepository repo;
        auto camera_opt = repo.getCameraById(id);
        if (camera_opt) {
            auto& camera = camera_opt.value();
            std::string url = camera.rtsp_url;
            auto pos = url.find("://");
            if (pos != std::string::npos) {
                std::string sub = url.substr(pos + 3);
                auto at_pos = sub.find("@");
                std::string credentials = "";
                std::string host_port = sub;
                if (at_pos != std::string::npos) {
                    credentials = sub.substr(0, at_pos);
                    host_port = sub.substr(at_pos + 1);
                    auto col_pos = credentials.find(":");
                    if (col_pos != std::string::npos) {
                        cfg.username = credentials.substr(0, col_pos);
                        cfg.password = credentials.substr(col_pos + 1);
                    }
                }
                auto slash_pos = host_port.find("/");
                std::string hp = (slash_pos == std::string::npos) ? host_port : host_port.substr(0, slash_pos);
                auto col_hp = hp.find(":");
                if (col_hp != std::string::npos) {
                    cfg.host = hp.substr(0, col_hp);
                    cfg.rtsp_port = std::stoi(hp.substr(col_hp + 1));
                } else {
                    cfg.host = hp;
                    cfg.rtsp_port = 554;
                }
            }
            cfg.http_port = 80;

            std::string lower = camera.name + " " + camera.description;
            for (auto& c : lower) c = tolower(c);
            if (lower.find("hanwha") != std::string::npos || lower.find("samsung") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Hanwha;
            else if (lower.find("axis") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Axis;
            else if (lower.find("bosch") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Bosch;
            else if (lower.find("hik") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Hikvision;
            else if (lower.find("dahua") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Dahua;
            else if (lower.find("uniview") != std::string::npos || lower.find("unv") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Uniview;
            else if (lower.find("pelco") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Pelco;
            else cfg.brand = CameraDiscovery::Brand::ONVIF;
        }
    } catch (...) {}
    return cfg;
}

bool CameraEventService::sendPTZCommand(const std::string& cam_id, const std::string& action, double speed) {
    CameraDiscovery::DiscoveryConfig cfg = getCameraConfig(cam_id);
    if (cfg.host.empty()) return false;
    
    auto core = brands::CoreFactory::getCore(cfg.brand);
    if (core) {
        LOG_INFO("CameraEventService PTZ {} to camera {} via ISAPI/CGI", action, cam_id);
        return core->ptzControl(cfg, action, speed);
    }
    return false;
}

void CameraEventService::workerLoop(std::shared_ptr<EventSession> session) {
    // Exponential reconnect backoff. Pre-fix every reconnect path slept a
    // flat 5-10s, so a misconfigured camera (wrong password, blocked port,
    // wrong brand) spun 720 attempts/hour at the device — operators
    // reported "VMS DoS'ing my NVR". Sequence escalates on each consecutive
    // failure and resets the moment we receive a real event (proves the
    // brand path actually worked at least once this cycle).
    //
    // Schedule: 5s, 10s, 30s, 60s, 300s (capped). Counter local to the
    // worker — survives across iterations within this session, no need to
    // expose on the session shape.
    constexpr int kBackoffSchedule[] = {5, 10, 30, 60, 300};
    constexpr int kBackoffCount = static_cast<int>(sizeof(kBackoffSchedule) / sizeof(kBackoffSchedule[0]));
    int consecutive_failures = 0;
    auto sleepBackoff = [&]() {
        const int idx = std::min(consecutive_failures, kBackoffCount - 1);
        const int secs = kBackoffSchedule[idx];
        std::this_thread::sleep_for(std::chrono::seconds(secs));
    };

    while (!session->stop_flag) {
        CameraDiscovery::DiscoveryConfig cfg = getCameraConfig(session->cam_id);
        if (cfg.host.empty()) {
            session->state = static_cast<int>(SubscriptionState::Failed);
            ++consecutive_failures;
            sleepBackoff();
            continue;
        }

        auto core = brands::CoreFactory::getCore(cfg.brand);
        if (!core) {
            session->state = static_cast<int>(SubscriptionState::Failed);
            ++consecutive_failures;
            sleepBackoff();
            continue;
        }

        // Record brand once (immutable after first set) so status snapshots
        // can show which protocol path is active without parsing cfg again.
        if (session->brand.empty()) {
            session->brand = core->getBrandName();
        }
        session->last_attempt_at = (long long)std::time(nullptr);
        session->state = static_cast<int>(SubscriptionState::Active);

        // Snapshot event counter before pullEvents so we can tell whether
        // the cycle actually produced events (counted as success → reset
        // backoff) vs. pullEvents returned without any (counted as failure
        // → escalate).
        const uint64_t events_before = session->total_events.load(std::memory_order_relaxed);

        LOG_INFO("CameraEventService polling events for {} (brand={})",
                 session->cam_id, session->brand);
        
        // BUG-EVENTS-01 followup (2026-05-12): consumer pre-fix only handled
        // the legacy Hikvision raw-XML `EventNotificationAlert` substring shape
        // and only inserted a TrafficCount row — Event rows were never created,
        // so EventsView never saw hardware alarms and RuleEngine never fired
        // alerts on them. Two parallel half-dead paths converging at the same
        // dead-end. Now: try JSON envelope first (new brand impls emit that),
        // fall back to legacy substring matching for backwards-compat with any
        // Hikvision adapter variant still emitting raw XML, and inject an
        // Event row so event_manager's broadcast loop picks it up + dispatches
        // through RuleEngine → AlertRouter / AlertManager naturally.
        core->pullEvents(cfg, [session](const std::string& chunk) -> bool {
            if (session->stop_flag) return false;
            if (chunk.empty()) return !session->stop_flag;

            std::string evt_type;
            bool active = true;
            int channel = 0;
            std::string brand;
            std::string raw_topic;

            // Attempt JSON envelope parse — the modern path. New brand impls
            // (Dahua/Axis/Hanwha/ONVIF) and the patched Hikvision adapter all
            // emit `{type, brand, event_type, active, channel, ...}`.
            bool parsed_envelope = false;
            try {
                auto j = nlohmann::json::parse(chunk);
                if (j.is_object() && j.value("type", std::string()) == "camera_event") {
                    evt_type  = j.value("event_type", std::string("hardware_alarm"));
                    active    = j.value("active",     true);
                    channel   = j.value("channel",    0);
                    brand     = j.value("brand",      std::string());
                    raw_topic = j.value("topic",      j.value("native_code", std::string()));
                    parsed_envelope = true;
                }
            } catch (...) {
                // Not JSON — fall through to legacy XML substring match.
            }

            // Legacy Hikvision raw-XML fallback (kept for adapters not yet
            // converted to the JSON envelope).
            if (!parsed_envelope) {
                if (chunk.size() < 50 || chunk.find("EventNotificationAlert") == std::string::npos) {
                    return !session->stop_flag;
                }
                evt_type = "hardware_alarm";
                if      (chunk.find("VMD")            != std::string::npos) evt_type = "motion_detect";
                else if (chunk.find("linedetection")  != std::string::npos) evt_type = "line_crossing";
                else if (chunk.find("fielddetection") != std::string::npos) evt_type = "intrusion";
                else if (chunk.find("facedetection")  != std::string::npos) evt_type = "face";
                else if (chunk.find("shelteralarm")   != std::string::npos) evt_type = "tampering";
                brand = "Hikvision";
            }

            // Only `active=true` transitions surface as new events. Edge-off
            // (active=false from Dahua Stop / ONVIF false) are not stored —
            // they would otherwise double the row count and confuse operators
            // who expect "1 row = 1 alarm raised".
            if (!active) {
                LOG_DEBUG("CameraEventService: ignoring inactive transition for {} cam={}",
                          evt_type, session->cam_id);
                return !session->stop_flag;
            }

            int camera_id = 0;
            try { camera_id = std::stoi(session->cam_id); } catch (...) {}

            const std::time_t now_ts = std::time(nullptr);

            // BUG-EVENT-DISPATCH-BYPASS-01 fix (2026-05-12): pre-fix this site
            // wrote the Event row via DbManager::enqueueEvent + did its own WS
            // broadcast inline. The original comment claimed event_manager.cpp
            // would pick it up and fire RuleEngine + AlertManager — that claim
            // was WRONG. Only EventManager::createEvent fires those, and
            // enqueueEvent bypasses createEvent entirely. So hardware alarms
            // appeared in EventsView but never triggered operator-configured
            // webhook/email/Telegram rules. Same shape bug as ZmqEventBridge
            // (now fixed in zmq_event_bridge.cpp::dispatchHardwareEvent helper).
            //
            // Fix: funnel through EventManager::createEvent which handles WS
            // broadcast + RuleEngine::evaluateEvent + AlertManager::processEvent
            // inline. Drop the redundant inline WS broadcast.
            //
            // Metadata enrichment: we keep the brand/channel/native_topic info
            // in metadata_json so EventsView can show the source brand and
            // RuleEngine condition expressions can pattern-match on those
            // fields if operators want hardware-specific rules.
            nlohmann::json metadata = {
                {"camera_id",   camera_id},
                {"channel",     channel},
                {"brand",       brand},
                {"event_type",  evt_type},
                {"severity",    severityForEventType(evt_type)},
                {"description", descriptionForEventType(evt_type, brand)}
            };
            if (!raw_topic.empty()) metadata["native"] = raw_topic;

            try {
                vms::Event evt;
                evt.camera_id     = camera_id;
                evt.event_type    = evt_type;
                evt.description   = descriptionForEventType(evt_type, brand);
                evt.severity      = severityForEventType(evt_type);
                evt.timestamp     = now_ts;
                evt.metadata_json = metadata.dump();
                evt.snapshot_path = ""; // Brand events don't carry frames today.
                vms::core::EventManager::getInstance().createEvent(evt);
            } catch (const std::exception& e) {
                LOG_ERROR("CameraEventService: failed to create Event for {} cam={}: {}",
                          evt_type, session->cam_id, e.what());
            }

            // Health counter: an event made it all the way through the
            // envelope-parse → DB-enqueue → WS-broadcast path. Use this to
            // distinguish "subscription is healthy but no alarms" from
            // "subscription is wedged" in the status endpoint.
            session->total_events.fetch_add(1, std::memory_order_relaxed);
            session->last_event_at = (long long)std::time(nullptr);

            LOG_INFO("CameraEventService intercepted hardware alarm: brand={} type={} cam={} channel={}",
                     brand, evt_type, camera_id, channel);
            return !session->stop_flag;
        });

        // pullEvents returned. If the caller didn't stop us, the brand stub
        // exited (ONVIF subscription expired, Hanwha poll hit an error,
        // Dahua disconnect, etc.). Sleep with exponential backoff before
        // reconnect — reset to 0 only if at least one event flowed this
        // cycle (proves credentials + network + parse are all working).
        if (!session->stop_flag) {
            session->state = static_cast<int>(SubscriptionState::Failed);
            session->reconnect_count.fetch_add(1, std::memory_order_relaxed);
            const uint64_t events_after = session->total_events.load(std::memory_order_relaxed);
            if (events_after > events_before) {
                consecutive_failures = 0;
            } else {
                ++consecutive_failures;
            }
            sleepBackoff();
        }
    }
    session->state = static_cast<int>(SubscriptionState::Stopped);
}

} // namespace core
} // namespace vms
