// ==============================================================
// File: include/events/event_types.h
// Event Data Model & Type Definitions
// ==============================================================

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>

namespace vms::events {

// ============================================================================
// EVENT ENUMS
// ============================================================================

enum class EventType {
    PERSON_DETECTED,
    VEHICLE_DETECTED,
    INTRUSION,              // Person in restricted zone
    LOITERING,              // Person stationary >30s
    CROWD_DETECTED,         // Multiple people in zone
    OBJECT_LEFT_BEHIND,     // Unattended object
    FIGHTING,               // Violence detection
    FALL_DETECTED,          // Person fell down
    FACE_RECOGNIZED,        // Known person identified
    LICENSE_PLATE,          // Vehicle plate detected
    LINE_CROSSING,          // Person crossed virtual line
    TAILGATING,             // Following too close
    FIRE_DETECTED,          // Fire detection
    SMOKE_DETECTED,         // Smoke detection
    // --- NEW: WP1 additions ---
    PPE_VIOLATION,          // Missing helmet/vest/glasses
    RUNNING,                // Speed anomaly detection
    SLEEPING,               // Person sleeping/inactive
    GATHERING,              // Group forming (>N people)
    WRONG_DIRECTION,        // Moving against allowed direction
    LINE_CROSSING_A_TO_B,   // Directional line crossing A→B
    LINE_CROSSING_B_TO_A,   // Directional line crossing B→A
    CAMERA_TAMPER,          // Camera covered/spray
    CAMERA_MOVED,           // Camera angle changed
    SIGNAL_LOSS,            // Video signal lost
    SCENE_CHANGE,           // Background changed significantly
    PERSON_REIDENTIFIED,    // Cross-camera Re-ID match
    UNKNOWN
};

enum class EventSeverity {
    LOW,        // Normal activity
    MEDIUM,     // Attention needed
    HIGH,       // Urgent
    CRITICAL    // Immediate action required
};

const char* eventTypeToString(EventType type);
const char* eventSeverityToString(EventSeverity severity);
EventType stringToEventType(const std::string& str);
EventSeverity stringToEventSeverity(const std::string& str);

// ============================================================================
// RAW EVENT (Single detection from one camera)
// ============================================================================

struct RawEvent {
    // Identity
    uint64_t event_id{0};              // Unique ID
    int camera_id{0};                  // Source camera
    EventType type{EventType::UNKNOWN};
    EventSeverity severity{EventSeverity::LOW};
    
    // Location
    int zone_id{-1};                   // Zone where event occurred (-1 = unknown)
    cv::Rect bbox;                     // Bounding box in frame coordinates
    cv::Point2f world_position;        // World coordinates (meters)
    
    // Object information
    std::string object_class;          // "person", "car", "bicycle"
    int track_id{-1};                  // Tracking ID from AI (-1 = no tracking)
    float confidence{0.0f};            // Detection confidence 0-1
    
    // Temporal
    std::chrono::system_clock::time_point timestamp;
    uint64_t frame_number{0};
    
    // Snapshot
    std::string snapshot_path;         // Path to JPEG snapshot
    std::vector<unsigned char> snapshot_data; // Or in-memory data
    
    // Metadata (flexible JSON for extra data)
    nlohmann::json metadata;           // Face embedding, plate text, etc.
    
    // Validation
    bool isValid() const {
        return event_id > 0 && camera_id >= 0 && confidence > 0.0f;
    }
    
    // Serialization
    nlohmann::json toJSON() const;
    static RawEvent fromJSON(const nlohmann::json& j);
};

// ============================================================================
// CORRELATED EVENT (Merged from multiple raw events)
// ============================================================================

struct CorrelatedEvent {
    // Identity
    uint64_t correlation_id{0};
    EventType type{EventType::UNKNOWN};
    EventSeverity severity{EventSeverity::LOW};
    
    // Source events
    std::vector<uint64_t> source_event_ids; // IDs of raw events
    std::vector<int> camera_ids;            // All cameras that saw this
    
    // Merged location
    int primary_zone_id{-1};
    std::vector<int> all_zones;             // Event may span multiple zones
    cv::Point2f centroid_position;          // Average world position
    
    // Temporal span
    std::chrono::system_clock::time_point first_seen;
    std::chrono::system_clock::time_point last_seen;
    std::chrono::milliseconds duration{0};
    
    // Statistics
    int event_count{0};                     // How many raw events merged
    float avg_confidence{0.0f};
    
    // Best snapshot (choose clearest/closest)
    std::string best_snapshot_path;
    int best_camera_id{-1};
    
    // Correlation metadata
    std::string correlation_reason;         // "spatial", "temporal", "tracking"
    nlohmann::json metadata;
    
    // State
    bool acknowledged{false};               // Operator has seen this
    std::chrono::system_clock::time_point acknowledged_at;
    std::string acknowledged_by;            // User who acknowledged
    
    // Serialization
    nlohmann::json toJSON() const;
    static CorrelatedEvent fromJSON(const nlohmann::json& j);
};

// ============================================================================
// EVENT STATISTICS
// ============================================================================

struct EventStatistics {
    // Counts
    uint64_t total_raw_events{0};
    uint64_t total_correlated_events{0};
    uint64_t active_correlations{0};
    
    // By type
    std::unordered_map<EventType, uint64_t> events_by_type;
    
    // By severity
    std::unordered_map<EventSeverity, uint64_t> events_by_severity;
    
    // By zone
    std::unordered_map<int, uint64_t> events_by_zone;
    
    // Performance
    float avg_correlation_latency_ms{0.0f};
    float reduction_percentage{0.0f}; // How much we reduced events
    
    // Time period
    std::chrono::system_clock::time_point period_start;
    std::chrono::system_clock::time_point period_end;
    
    nlohmann::json toJSON() const;
};

} // namespace vms::events
