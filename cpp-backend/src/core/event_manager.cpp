#include "core/event_manager.h"
#include "utils/logger.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <thread>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "database/db_manager.h"
#include "core/alert_manager.h"
#include "events/rule_engine.h"
#include "events/event_types.h"

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

    bool result = event_repo_->insertEvent(event);

    if (result) {
        LOG_DEBUG("Event created: {}", event.id);
        
        // --- Milestone 6: Multi-Rule Alert Dispatching ---
        try {
            vms::core::AlertManager::getInstance().processEvent(event);
        } catch (const std::exception& e) {
            LOG_ERROR("EventManager: AlertManager failed to process event: {}", e.what());
        }

        // --- Execute Advanced Rule Engine ---
        try {
            vms::events::RawEvent raw;
            raw.event_id = std::hash<std::string>{}(event.id);
            raw.camera_id = event.camera_id;
            
            // Map event type
            std::string type_lower = event.event_type;
            std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(), ::tolower);
            if (type_lower.find("face") != std::string::npos) {
                raw.type = vms::events::EventType::FACE_RECOGNIZED;
            } else if (type_lower == "person_detected" || type_lower == "intrusion") {
                raw.type = vms::events::EventType::INTRUSION;
            } else if (type_lower == "loitering") {
                raw.type = vms::events::EventType::LOITERING;
            } else if (type_lower == "ppe_violation" || type_lower == "ppe") {
                raw.type = vms::events::EventType::PPE_VIOLATION;
            } else {
                raw.type = vms::events::EventType::UNKNOWN;
            }
            
            raw.timestamp = std::chrono::system_clock::now();
            raw.severity = vms::events::EventSeverity::MEDIUM; // default
            raw.confidence = 1.0f;
            
            // Extract confidence from metadata
            if (!event.metadata_json.empty()) {
                try {
                    auto meta = nlohmann::json::parse(event.metadata_json);
                    if (meta.contains("confidence")) {
                        raw.confidence = meta["confidence"].get<float>();
                    }
                } catch(...) {}
            }
            
            raw.snapshot_path = event.snapshot_path;
            
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
