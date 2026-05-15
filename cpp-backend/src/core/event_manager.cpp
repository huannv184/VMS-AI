#include "core/event_manager.h"
#include "utils/logger.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <thread>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "database/db_manager.h"
#include "events/rule_engine.h"
#include "events/event_types.h"
#include "streaming/camera_stream_manager_qt.h"

namespace vms {
namespace core {

EventManager& EventManager::getInstance() {
    static EventManager instance;
    return instance;
}

bool EventManager::init() {
    LOG_INFO("Initializing EventManager...");
    
    event_repo_ = std::make_unique<database::EventRepository>();
    
    LOG_INFO("EventManager initialized");
    return true;
}

bool EventManager::createEvent(const Event& event) {
    LOG_DEBUG("Creating event: {} for camera {}", event.event_type, event.camera_id);

    if (!event_repo_) {
        LOG_THROTTLED_ERROR(5000, "EventManager not initialized");
        return false;
    }

    // Ensure every event has a unique ID before insertion.
    // Callers that omit event.id would otherwise insert with id="" causing all
    // subsequent events to be silently dropped by INSERT OR IGNORE / ON CONFLICT.
    Event ev = event;
    if (ev.id.empty()) {
        ev.id = generateEventId();
    }

    bool result = event_repo_->insertEvent(ev);

    if (result) {
        LOG_DEBUG("Event created: {}", ev.id);

        try {
            nlohmann::json ws_payload;
            ws_payload["type"]       = "alert";
            ws_payload["event_type"] = ev.event_type;
            ws_payload["id"]         = ev.id;
            ws_payload["camera_id"]  = ev.camera_id;
            ws_payload["timestamp"]  = ev.timestamp;
            ws_payload["description"]= ev.description;
            ws_payload["severity"]   = ev.severity;
            ws_payload["snapshot_url"] = ev.snapshot_path;

            if (!ev.metadata_json.empty()) {
                try {
                    auto meta = nlohmann::json::parse(ev.metadata_json);
                    if (meta.contains("confidence")) ws_payload["confidence"] = meta["confidence"];
                    if (meta.contains("bbox"))       ws_payload["bbox"]       = meta["bbox"];
                } catch (...) {}
            }

            auto& ws = vms::streaming::CameraStreamManager::getInstance();
            if (ev.camera_id > 0) {
                ws.broadcastEvent(ev.camera_id, ws_payload);
            }
            ws.broadcastEvent(0, ws_payload);
        } catch (const std::exception& e) {
            LOG_THROTTLED_ERROR(5000, "EventManager: WS broadcast failed: {}", e.what());
        }

        // 2026-05-14: removed AlertManager::processEvent dispatch (legacy
        // alert_rules table). Legacy rows are migrated to composite rules on
        // boot by RuleEngine::migrateLegacyAlertRules; delivery now flows
        // exclusively through RuleEngine → vms::events::deliverAction.

        try {
            static std::atomic<uint64_t> raw_event_id_counter{1};

            vms::events::RawEvent raw;
            raw.event_id = raw_event_id_counter.fetch_add(1, std::memory_order_relaxed);
            raw.camera_id = ev.camera_id;

            std::string type_upper = ev.event_type;
            std::transform(type_upper.begin(), type_upper.end(), type_upper.begin(), ::toupper);
            raw.type = vms::events::stringToEventType(type_upper);

            raw.timestamp = std::chrono::system_clock::now();
            raw.severity = vms::events::EventSeverity::MEDIUM;
            raw.confidence = 1.0f;

            // BUG-6 FIX: populate object_class / track_id / zone_id / bbox / world_position
            // from metadata so RuleEngine condition types OBJECT/ZONE/CAMERA/CONFIDENCE
            // can actually filter. Previously these stayed at defaults → conditions
            // referencing them never matched → AI rules silently no-oped.
            if (!ev.metadata_json.empty()) {
                try {
                    auto meta = nlohmann::json::parse(ev.metadata_json);
                    raw.metadata = meta;
                    if (meta.contains("confidence") && meta["confidence"].is_number()) {
                        raw.confidence = meta["confidence"].get<float>();
                    }
                    if (meta.contains("severity") && meta["severity"].is_string()) {
                        raw.severity = vms::events::stringToEventSeverity(meta["severity"].get<std::string>());
                    }
                    if (meta.contains("type") && meta["type"].is_string()) {
                        raw.object_class = meta["type"].get<std::string>();
                    } else if (meta.contains("class_name") && meta["class_name"].is_string()) {
                        raw.object_class = meta["class_name"].get<std::string>();
                    }
                    if (meta.contains("track_id") && meta["track_id"].is_number_integer()) {
                        raw.track_id = meta["track_id"].get<int>();
                    }
                    if (meta.contains("zone_id") && meta["zone_id"].is_number_integer()) {
                        raw.zone_id = meta["zone_id"].get<int>();
                    }
                    if (meta.contains("bbox") && meta["bbox"].is_array() && meta["bbox"].size() >= 4) {
                        int x1 = meta["bbox"][0].get<int>();
                        int y1 = meta["bbox"][1].get<int>();
                        int x2 = meta["bbox"][2].get<int>();
                        int y2 = meta["bbox"][3].get<int>();
                        raw.bbox = cv::Rect(x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1));
                    }
                } catch(...) {}
            }

            raw.snapshot_path = ev.snapshot_path;
            vms::events::RuleEngine::getInstance().evaluateEvent(raw);
        } catch (const std::exception& e) {
            LOG_ERROR("EventManager: RuleEngine failed to evaluate event: {}", e.what());
        }
    } else {
        LOG_ERROR("Failed to create event");
    }

    return result;
}

std::vector<Event> EventManager::getEvents(int camera_id,
                                           int limit,
                                           int offset,
                                           const std::string& event_type,
                                           std::optional<long long> start_time,
                                           std::optional<long long> end_time) {
    if (!event_repo_) {
        LOG_ERROR("EventManager not initialized");
        return {};
    }
    
    return event_repo_->getEvents(camera_id, limit, offset, event_type, start_time, end_time);
}

std::optional<Event> EventManager::getEvent(const std::string& event_id) {
    if (!event_repo_) {
        LOG_ERROR("EventManager not initialized");
        return std::nullopt;
    }
    
    return event_repo_->getEventById(event_id);
}

bool EventManager::deleteEvent(const std::string& event_id) {
    LOG_INFO("Deleting event: {}", event_id);
    
    if (!event_repo_) {
        LOG_ERROR("EventManager not initialized");
        return false;
    }
    
    bool result = event_repo_->deleteEvent(event_id);
    
    if (result) {
        LOG_INFO("Event deleted successfully: {}", event_id);
    } else {
        LOG_ERROR("Failed to delete event: {}", event_id);
    }
    
    return result;
}

int EventManager::getEventCount(int camera_id) {
    if (!event_repo_) {
        LOG_ERROR("EventManager not initialized");
        return 0;
    }
    
    return event_repo_->getEventCount(camera_id);
}

std::string EventManager::generateEventId() {
    // Standard UUID v4 generation
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    uint64_t part1 = dis(gen);
    uint64_t part2 = dis(gen);

    // Set version to 4
    part1 = (part1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Set variant to 10xx
    part2 = (part2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << (part1 >> 32) << "-";
    ss << std::setw(4) << ((part1 >> 16) & 0xFFFF) << "-";
    ss << std::setw(4) << (part1 & 0xFFFF) << "-";
    ss << std::setw(4) << (part2 >> 48) << "-";
    ss << std::setw(12) << (part2 & 0xFFFFFFFFFFFFULL);

    return ss.str();
}

} // namespace core
} // namespace vms
