// ==============================================================
// File: src/events/event_types.cpp
// Event Type Implementations
// ==============================================================

#include "events/event_types.h"
#include <algorithm>

namespace vms::events {

// ============================================================================
// STRING CONVERSION
// ============================================================================

const char* eventTypeToString(EventType type) {
    switch (type) {
        case EventType::PERSON_DETECTED: return "PERSON_DETECTED";
        case EventType::VEHICLE_DETECTED: return "VEHICLE_DETECTED";
        case EventType::INTRUSION: return "INTRUSION";
        case EventType::LOITERING: return "LOITERING";
        case EventType::CROWD_DETECTED: return "CROWD_DETECTED";
        case EventType::OBJECT_LEFT_BEHIND: return "OBJECT_LEFT_BEHIND";
        case EventType::FIGHTING: return "FIGHTING";
        case EventType::FALL_DETECTED: return "FALL_DETECTED";
        case EventType::FACE_RECOGNIZED: return "FACE_RECOGNIZED";
        case EventType::LICENSE_PLATE: return "LICENSE_PLATE";
        case EventType::LINE_CROSSING: return "LINE_CROSSING";
        case EventType::TAILGATING: return "TAILGATING";
        case EventType::FIRE_DETECTED: return "FIRE_DETECTED";
        case EventType::SMOKE_DETECTED: return "SMOKE_DETECTED";
        case EventType::PPE_VIOLATION: return "PPE_VIOLATION";
        case EventType::RUNNING: return "RUNNING";
        case EventType::SLEEPING: return "SLEEPING";
        case EventType::GATHERING: return "GATHERING";
        case EventType::WRONG_DIRECTION: return "WRONG_DIRECTION";
        case EventType::LINE_CROSSING_A_TO_B: return "LINE_CROSSING_A_TO_B";
        case EventType::LINE_CROSSING_B_TO_A: return "LINE_CROSSING_B_TO_A";
        case EventType::CAMERA_TAMPER: return "CAMERA_TAMPER";
        case EventType::CAMERA_MOVED: return "CAMERA_MOVED";
        case EventType::SIGNAL_LOSS: return "SIGNAL_LOSS";
        case EventType::SCENE_CHANGE: return "SCENE_CHANGE";
        case EventType::PERSON_REIDENTIFIED: return "PERSON_REIDENTIFIED";
        default: return "UNKNOWN";
    }
}

const char* eventSeverityToString(EventSeverity severity) {
    switch (severity) {
        case EventSeverity::LOW: return "LOW";
        case EventSeverity::MEDIUM: return "MEDIUM";
        case EventSeverity::HIGH: return "HIGH";
        case EventSeverity::CRITICAL: return "CRITICAL";
        default: return "LOW";
    }
}

EventType stringToEventType(const std::string& str) {
    if (str == "PERSON_DETECTED") return EventType::PERSON_DETECTED;
    if (str == "VEHICLE_DETECTED") return EventType::VEHICLE_DETECTED;
    if (str == "INTRUSION") return EventType::INTRUSION;
    if (str == "LOITERING") return EventType::LOITERING;
    if (str == "CROWD_DETECTED") return EventType::CROWD_DETECTED;
    if (str == "OBJECT_LEFT_BEHIND") return EventType::OBJECT_LEFT_BEHIND;
    if (str == "FIGHTING") return EventType::FIGHTING;
    if (str == "FALL_DETECTED") return EventType::FALL_DETECTED;
    if (str == "FACE_RECOGNIZED") return EventType::FACE_RECOGNIZED;
    if (str == "LICENSE_PLATE") return EventType::LICENSE_PLATE;
    if (str == "LINE_CROSSING") return EventType::LINE_CROSSING;
    if (str == "TAILGATING") return EventType::TAILGATING;
    if (str == "FIRE_DETECTED") return EventType::FIRE_DETECTED;
    if (str == "SMOKE_DETECTED") return EventType::SMOKE_DETECTED;
    if (str == "PPE_VIOLATION") return EventType::PPE_VIOLATION;
    if (str == "RUNNING") return EventType::RUNNING;
    if (str == "SLEEPING") return EventType::SLEEPING;
    if (str == "GATHERING") return EventType::GATHERING;
    if (str == "WRONG_DIRECTION") return EventType::WRONG_DIRECTION;
    if (str == "LINE_CROSSING_A_TO_B") return EventType::LINE_CROSSING_A_TO_B;
    if (str == "LINE_CROSSING_B_TO_A") return EventType::LINE_CROSSING_B_TO_A;
    if (str == "CAMERA_TAMPER") return EventType::CAMERA_TAMPER;
    if (str == "CAMERA_MOVED") return EventType::CAMERA_MOVED;
    if (str == "SIGNAL_LOSS") return EventType::SIGNAL_LOSS;
    if (str == "SCENE_CHANGE") return EventType::SCENE_CHANGE;
    if (str == "PERSON_REIDENTIFIED") return EventType::PERSON_REIDENTIFIED;
    return EventType::UNKNOWN;
}

EventSeverity stringToEventSeverity(const std::string& str) {
    if (str == "LOW") return EventSeverity::LOW;
    if (str == "MEDIUM") return EventSeverity::MEDIUM;
    if (str == "HIGH") return EventSeverity::HIGH;
    if (str == "CRITICAL") return EventSeverity::CRITICAL;
    return EventSeverity::LOW;
}

// ============================================================================
// RAW EVENT SERIALIZATION
// ============================================================================

nlohmann::json RawEvent::toJSON() const {
    nlohmann::json j;
    
    j["event_id"] = event_id;
    j["camera_id"] = camera_id;
    j["type"] = eventTypeToString(type);
    j["severity"] = eventSeverityToString(severity);
    j["zone_id"] = zone_id;
    
    j["bbox"] = {
        {"x", bbox.x},
        {"y", bbox.y},
        {"width", bbox.width},
        {"height", bbox.height}
    };
    
    j["world_position"] = {
        {"x", world_position.x},
        {"y", world_position.y}
    };
    
    j["object_class"] = object_class;
    j["track_id"] = track_id;
    j["confidence"] = confidence;
    
    auto time_t_val = std::chrono::system_clock::to_time_t(timestamp);
    j["timestamp"] = time_t_val;
    j["frame_number"] = frame_number;
    
    j["snapshot_path"] = snapshot_path;
    j["metadata"] = metadata;
    
    return j;
}

RawEvent RawEvent::fromJSON(const nlohmann::json& j) {
    RawEvent event;
    
    event.event_id = j.value("event_id", 0ULL);
    event.camera_id = j.value("camera_id", 0);
    event.type = stringToEventType(j.value("type", "UNKNOWN"));
    event.severity = stringToEventSeverity(j.value("severity", "LOW"));
    event.zone_id = j.value("zone_id", -1);
    
    if (j.contains("bbox")) {
        event.bbox.x = j["bbox"].value("x", 0);
        event.bbox.y = j["bbox"].value("y", 0);
        event.bbox.width = j["bbox"].value("width", 0);
        event.bbox.height = j["bbox"].value("height", 0);
    }
    
    if (j.contains("world_position")) {
        event.world_position.x = j["world_position"].value("x", 0.0f);
        event.world_position.y = j["world_position"].value("y", 0.0f);
    }
    
    event.object_class = j.value("object_class", "");
    event.track_id = j.value("track_id", -1);
    event.confidence = j.value("confidence", 0.0f);
    
    if (j.contains("timestamp")) {
        std::time_t time_t_val = j["timestamp"];
        event.timestamp = std::chrono::system_clock::from_time_t(time_t_val);
    }
    
    event.frame_number = j.value("frame_number", 0ULL);
    event.snapshot_path = j.value("snapshot_path", "");
    
    if (j.contains("metadata")) {
        event.metadata = j["metadata"];
    }
    
    return event;
}

// ============================================================================
// CORRELATED EVENT SERIALIZATION
// ============================================================================

nlohmann::json CorrelatedEvent::toJSON() const {
    nlohmann::json j;
    
    j["correlation_id"] = correlation_id;
    j["type"] = eventTypeToString(type);
    j["severity"] = eventSeverityToString(severity);
    
    j["source_event_ids"] = source_event_ids;
    j["camera_ids"] = camera_ids;
    j["primary_zone_id"] = primary_zone_id;
    j["all_zones"] = all_zones;
    
    j["centroid_position"] = {
        {"x", centroid_position.x},
        {"y", centroid_position.y}
    };
    
    j["first_seen"] = std::chrono::system_clock::to_time_t(first_seen);
    j["last_seen"] = std::chrono::system_clock::to_time_t(last_seen);
    j["duration_ms"] = duration.count();
    
    j["event_count"] = event_count;
    j["avg_confidence"] = avg_confidence;
    j["best_snapshot_path"] = best_snapshot_path;
    j["best_camera_id"] = best_camera_id;
    j["correlation_reason"] = correlation_reason;
    
    j["acknowledged"] = acknowledged;
    if (acknowledged) {
        j["acknowledged_at"] = std::chrono::system_clock::to_time_t(acknowledged_at);
        j["acknowledged_by"] = acknowledged_by;
    }
    
    j["metadata"] = metadata;
    
    return j;
}

CorrelatedEvent CorrelatedEvent::fromJSON(const nlohmann::json& j) {
    CorrelatedEvent event;
    
    event.correlation_id = j.value("correlation_id", 0ULL);
    event.type = stringToEventType(j.value("type", "UNKNOWN"));
    event.severity = stringToEventSeverity(j.value("severity", "LOW"));
    
    if (j.contains("source_event_ids")) {
        event.source_event_ids = j["source_event_ids"].get<std::vector<uint64_t>>();
    }
    
    if (j.contains("camera_ids")) {
        event.camera_ids = j["camera_ids"].get<std::vector<int>>();
    }
    
    event.primary_zone_id = j.value("primary_zone_id", -1);
    
    if (j.contains("all_zones")) {
        event.all_zones = j["all_zones"].get<std::vector<int>>();
    }
    
    if (j.contains("centroid_position")) {
        event.centroid_position.x = j["centroid_position"].value("x", 0.0f);
        event.centroid_position.y = j["centroid_position"].value("y", 0.0f);
    }
    
    if (j.contains("first_seen")) {
        event.first_seen = std::chrono::system_clock::from_time_t(j["first_seen"]);
    }
    
    if (j.contains("last_seen")) {
        event.last_seen = std::chrono::system_clock::from_time_t(j["last_seen"]);
    }
    
    event.duration = std::chrono::milliseconds(j.value("duration_ms", 0));
    event.event_count = j.value("event_count", 0);
    event.avg_confidence = j.value("avg_confidence", 0.0f);
    event.best_snapshot_path = j.value("best_snapshot_path", "");
    event.best_camera_id = j.value("best_camera_id", -1);
    event.correlation_reason = j.value("correlation_reason", "");
    event.acknowledged = j.value("acknowledged", false);
    
    if (event.acknowledged && j.contains("acknowledged_at")) {
        event.acknowledged_at = std::chrono::system_clock::from_time_t(j["acknowledged_at"]);
        event.acknowledged_by = j.value("acknowledged_by", "");
    }
    
    if (j.contains("metadata")) {
        event.metadata = j["metadata"];
    }
    
    return event;
}

// ============================================================================
// STATISTICS SERIALIZATION
// ============================================================================

nlohmann::json EventStatistics::toJSON() const {
    nlohmann::json j;
    
    j["total_raw_events"] = total_raw_events;
    j["total_correlated_events"] = total_correlated_events;
    j["active_correlations"] = active_correlations;
    j["avg_correlation_latency_ms"] = avg_correlation_latency_ms;
    j["reduction_percentage"] = reduction_percentage;
    
    // By type
    nlohmann::json by_type = nlohmann::json::object();
    for (const auto& [type, count] : events_by_type) {
        by_type[eventTypeToString(type)] = count;
    }
    j["events_by_type"] = by_type;
    
    // By severity
    nlohmann::json by_severity = nlohmann::json::object();
    for (const auto& [severity, count] : events_by_severity) {
        by_severity[eventSeverityToString(severity)] = count;
    }
    j["events_by_severity"] = by_severity;
    
    // By zone
    j["events_by_zone"] = events_by_zone;
    
    j["period_start"] = std::chrono::system_clock::to_time_t(period_start);
    j["period_end"] = std::chrono::system_clock::to_time_t(period_end);
    
    return j;
}

} // namespace vms::events
