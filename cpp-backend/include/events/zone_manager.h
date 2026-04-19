// ==============================================================
// File: include/events/zone_manager.h
// Zone Management for Event Correlation
// ==============================================================

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>

namespace vms::events {

// ============================================================================
// ZONE DEFINITION
// ============================================================================

struct Zone {
    // Identity
    int zone_id{0};
    std::string name;                   // "Front Lobby", "Parking Lot A"
    std::string type{"public"};         // "public", "restricted", "secure"
    std::string description;
    
    // Geographic boundary (polygon in world coordinates)
    std::vector<cv::Point2f> polygon;
    
    // Cameras covering this zone
    std::vector<int> camera_ids;
    
    // Rules & limits
    int max_occupancy{-1};              // Max people allowed (-1 = unlimited)
    bool allow_loitering{true};
    int loitering_threshold_sec{30};
    
    // Alert settings
    bool enable_alerts{true};
    int max_alerts_per_minute{5};
    
    // Metadata
    nlohmann::json metadata;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    
    // Check if point is inside zone
    bool containsPoint(const cv::Point2f& point) const;
    
    // Serialization
    nlohmann::json toJSON() const;
    static Zone fromJSON(const nlohmann::json& j);
};

// ============================================================================
// ZONE MANAGER (Singleton)
// ============================================================================

class ZoneManager {
public:
    static ZoneManager& getInstance();
    
    // Zone CRUD
    bool addZone(const Zone& zone);
    bool removeZone(int zone_id);
    bool updateZone(const Zone& zone);
    std::shared_ptr<Zone> getZone(int zone_id);
    std::vector<Zone> getAllZones();
    
    // Queries
    std::vector<int> getZonesForCamera(int camera_id);
    int findZoneForPosition(const cv::Point2f& world_pos);
    std::vector<int> findZonesForPosition(const cv::Point2f& world_pos); // Can be in multiple zones
    bool isInRestrictedZone(const cv::Point2f& world_pos);
    
    // Occupancy tracking
    int getCurrentOccupancy(int zone_id);
    void updateOccupancy(int zone_id, int count);
    void incrementOccupancy(int zone_id);
    void decrementOccupancy(int zone_id);
    
    // Alert rate limiting
    bool canSendAlert(int zone_id); // Check if within rate limit
    void recordAlert(int zone_id);  // Record that alert was sent
    
    // Statistics
    nlohmann::json getZoneStatistics(int zone_id);
    nlohmann::json getAllStatistics();
    
    // Load/Save from database
    bool loadFromDatabase();
    bool saveToDatabase();
    
private:
    ZoneManager() = default;
    ~ZoneManager() = default;
    
    // Prevent copying
    ZoneManager(const ZoneManager&) = delete;
    ZoneManager& operator=(const ZoneManager&) = delete;
    
    // Data
    std::unordered_map<int, Zone> zones_;
    std::unordered_map<int, int> zone_occupancy_;
    
    // Alert rate limiting tracking
    struct AlertRateInfo {
        std::vector<std::chrono::system_clock::time_point> recent_alerts;
    };
    std::unordered_map<int, AlertRateInfo> alert_rates_;
    
    // Thread safety
    mutable std::mutex mutex_;
    
    // Helper: Point in polygon test
    static bool pointInPolygon(const cv::Point2f& point, const std::vector<cv::Point2f>& polygon);
};

} // namespace vms::events
