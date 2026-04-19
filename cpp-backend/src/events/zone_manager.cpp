// ==============================================================
// File: src/events/zone_manager.cpp
// Zone Manager Implementation
// ==============================================================

#include "events/zone_manager.h"
#include "utils/logger.h"
#include "database/db_manager.h"
#include <algorithm>

#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QString>

namespace vms::events {

// ============================================================================
// ZONE METHODS
// ============================================================================

bool Zone::containsPoint(const cv::Point2f& point) const {
    if (polygon.empty()) return false;
    
    // Use ray casting algorithm
    bool inside = false;
    int n = polygon.size();
    
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((polygon[i].y > point.y) != (polygon[j].y > point.y)) &&
            (point.x < (polygon[j].x - polygon[i].x) * 
                      (point.y - polygon[i].y) / 
                      (polygon[j].y - polygon[i].y) + polygon[i].x)) {
            inside = !inside;
        }
    }
    
    return inside;
}

nlohmann::json Zone::toJSON() const {
    nlohmann::json j;
    
    j["zone_id"] = zone_id;
    j["name"] = name;
    j["type"] = type;
    j["description"] = description;
    
    // Polygon
    nlohmann::json poly_array = nlohmann::json::array();
    for (const auto& pt : polygon) {
        poly_array.push_back({{"x", pt.x}, {"y", pt.y}});
    }
    j["polygon"] = poly_array;
    
    j["camera_ids"] = camera_ids;
    j["max_occupancy"] = max_occupancy;
    j["allow_loitering"] = allow_loitering;
    j["loitering_threshold_sec"] = loitering_threshold_sec;
    j["enable_alerts"] = enable_alerts;
    j["max_alerts_per_minute"] = max_alerts_per_minute;
    j["metadata"] = metadata;
    
    return j;
}

Zone Zone::fromJSON(const nlohmann::json& j) {
    Zone zone;
    
    zone.zone_id = j.value("zone_id", 0);
    zone.name = j.value("name", "");
    zone.type = j.value("type", "public");
    zone.description = j.value("description", "");
    
    if (j.contains("polygon") && j["polygon"].is_array()) {
        for (const auto& pt : j["polygon"]) {
            zone.polygon.push_back({
                pt.value("x", 0.0f),
                pt.value("y", 0.0f)
            });
        }
    }
    
    if (j.contains("camera_ids")) {
        zone.camera_ids = j["camera_ids"].get<std::vector<int>>();
    }
    
    zone.max_occupancy = j.value("max_occupancy", -1);
    zone.allow_loitering = j.value("allow_loitering", true);
    zone.loitering_threshold_sec = j.value("loitering_threshold_sec", 30);
    zone.enable_alerts = j.value("enable_alerts", true);
    zone.max_alerts_per_minute = j.value("max_alerts_per_minute", 5);
    
    if (j.contains("metadata")) {
        zone.metadata = j["metadata"];
    }
    
    return zone;
}

// ============================================================================
// ZONE MANAGER
// ============================================================================

ZoneManager& ZoneManager::getInstance() {
    static ZoneManager instance;
    return instance;
}

bool ZoneManager::addZone(const Zone& zone) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (zones_.find(zone.zone_id) != zones_.end()) {
        LOG_WARN("[ZoneManager] Zone {} already exists", zone.zone_id);
        return false;
    }
    
    zones_[zone.zone_id] = zone;
    zone_occupancy_[zone.zone_id] = 0;
    
    LOG_INFO("[ZoneManager] Added zone {}: {}", zone.zone_id, zone.name);
    return true;
}

bool ZoneManager::removeZone(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = zones_.find(zone_id);
    if (it == zones_.end()) {
        LOG_WARN("[ZoneManager] Zone {} not found", zone_id);
        return false;
    }
    
    zones_.erase(it);
    zone_occupancy_.erase(zone_id);
    alert_rates_.erase(zone_id);
    
    LOG_INFO("[ZoneManager] Removed zone {}", zone_id);
    return true;
}

bool ZoneManager::updateZone(const Zone& zone) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = zones_.find(zone.zone_id);
    if (it == zones_.end()) {
        LOG_WARN("[ZoneManager] Zone {} not found for update", zone.zone_id);
        return false;
    }
    
    it->second = zone;
    LOG_INFO("[ZoneManager] Updated zone {}", zone.zone_id);
    return true;
}

std::shared_ptr<Zone> ZoneManager::getZone(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = zones_.find(zone_id);
    if (it == zones_.end()) {
        return nullptr;
    }
    
    // Return a copy wrapped in shared_ptr to avoid race conditions with map modification
    return std::make_shared<Zone>(it->second);
}

std::vector<Zone> ZoneManager::getAllZones() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Zone> result;
    for (const auto& [id, zone] : zones_) {
        result.push_back(zone);
    }
    
    return result;
}

std::vector<int> ZoneManager::getZonesForCamera(int camera_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<int> result;
    for (const auto& [id, zone] : zones_) {
        if (std::find(zone.camera_ids.begin(), zone.camera_ids.end(), 
                     camera_id) != zone.camera_ids.end()) {
            result.push_back(id);
        }
    }
    
    return result;
}

int ZoneManager::findZoneForPosition(const cv::Point2f& world_pos) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& [id, zone] : zones_) {
        if (zone.containsPoint(world_pos)) {
            return id;
        }
    }
    
    return -1; // Not in any zone
}

std::vector<int> ZoneManager::findZonesForPosition(const cv::Point2f& world_pos) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<int> result;
    for (const auto& [id, zone] : zones_) {
        if (zone.containsPoint(world_pos)) {
            result.push_back(id);
        }
    }
    
    return result;
}

bool ZoneManager::isInRestrictedZone(const cv::Point2f& world_pos) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& [id, zone] : zones_) {
        if (zone.type == "restricted" || zone.type == "secure") {
            if (zone.containsPoint(world_pos)) {
                return true;
            }
        }
    }
    
    return false;
}

int ZoneManager::getCurrentOccupancy(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = zone_occupancy_.find(zone_id);
    if (it == zone_occupancy_.end()) {
        return 0;
    }
    
    return it->second;
}

void ZoneManager::updateOccupancy(int zone_id, int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    zone_occupancy_[zone_id] = count;
    
    // Log if exceeds max occupancy
    auto zone_it = zones_.find(zone_id);
    if (zone_it != zones_.end() && zone_it->second.max_occupancy > 0) {
        if (count > zone_it->second.max_occupancy) {
            LOG_WARN("[ZoneManager] Zone {} occupancy {} exceeds max {}", 
                    zone_id, count, zone_it->second.max_occupancy);
        }
    }
}

void ZoneManager::incrementOccupancy(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    zone_occupancy_[zone_id]++;
}

void ZoneManager::decrementOccupancy(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (zone_occupancy_[zone_id] > 0) {
        zone_occupancy_[zone_id]--;
    }
}

bool ZoneManager::canSendAlert(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto zone_it = zones_.find(zone_id);
    if (zone_it == zones_.end()) {
        return false;
    }
    
    if (!zone_it->second.enable_alerts) {
        return false;
    }
    
    // Check rate limit
    auto& rate_info = alert_rates_[zone_id];
    auto now = std::chrono::system_clock::now();
    auto one_minute_ago = now - std::chrono::minutes(1);
    
    // Remove old alerts
    rate_info.recent_alerts.erase(
        std::remove_if(rate_info.recent_alerts.begin(), 
                      rate_info.recent_alerts.end(),
                      [one_minute_ago](const auto& t) { return t < one_minute_ago; }),
        rate_info.recent_alerts.end()
    );
    
    // Check if under limit
    int max_per_min = zone_it->second.max_alerts_per_minute;
    return rate_info.recent_alerts.size() < static_cast<size_t>(max_per_min);
}

void ZoneManager::recordAlert(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& rate_info = alert_rates_[zone_id];
    rate_info.recent_alerts.push_back(std::chrono::system_clock::now());
}

nlohmann::json ZoneManager::getZoneStatistics(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    nlohmann::json stats;
    
    auto zone_it = zones_.find(zone_id);
    if (zone_it == zones_.end()) {
        stats["error"] = "Zone not found";
        return stats;
    }
    
    auto& zone = zone_it->second;
    
    stats["zone_id"] = zone_id;
    stats["name"] = zone.name;
    stats["type"] = zone.type;
    stats["current_occupancy"] = getCurrentOccupancy(zone_id);
    stats["max_occupancy"] = zone.max_occupancy;
    
    if (zone.max_occupancy > 0) {
        stats["occupancy_percentage"] = 
            (getCurrentOccupancy(zone_id) * 100.0f) / zone.max_occupancy;
    }
    
    // Recent alerts
    auto& rate_info = alert_rates_[zone_id];
    stats["alerts_last_minute"] = rate_info.recent_alerts.size();
    
    return stats;
}

nlohmann::json ZoneManager::getAllStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    nlohmann::json all_stats = nlohmann::json::array();
    
    for (const auto& [zone_id, zone] : zones_) {
        all_stats.push_back(getZoneStatistics(zone_id));
    }
    
    return all_stats;
}

bool ZoneManager::loadFromDatabase() {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_INFO("[ZoneManager] Loading zones from database...");
    
    auto& db_mgr = database::DbManager::getInstance();
    if (!db_mgr.isInitialized()) return false;
    
    QSqlDatabase db = db_mgr.getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_ERROR("[ZoneManager] Failed to get database connection");
        return false;
    }
    
    QSqlQuery query(db);
    if (!query.exec("SELECT id, name, type, description, polygon_json, camera_ids_json, "
                    "max_occupancy, allow_loitering, loitering_threshold_sec, "
                    "enable_alerts, max_alerts_per_minute, metadata_json, created_at, updated_at "
                    "FROM zones")) {
        LOG_ERROR("[ZoneManager] Failed to load zones: {}", query.lastError().text().toStdString());
        return false;
    }
    
    zones_.clear();
    
    while (query.next()) {
        Zone z;
        z.zone_id = query.value(0).toInt();
        z.name = query.value(1).toString().toStdString();
        z.type = query.value(2).toString().toStdString();
        
        QVariant desc_val = query.value(3);
        if (!desc_val.isNull()) z.description = desc_val.toString().toStdString();
        
        // Parse polygon JSON
        QVariant poly_val = query.value(4);
        if (!poly_val.isNull()) {
            try {
                auto poly_j = nlohmann::json::parse(poly_val.toString().toStdString());
                for (auto& pt : poly_j) {
                    z.polygon.push_back(cv::Point2f(pt["x"].get<float>(), pt["y"].get<float>()));
                }
            } catch(...) {}
        }
        
        // Parse camera_ids JSON
        QVariant cams_val = query.value(5);
        if (!cams_val.isNull()) {
            try {
                auto cams_j = nlohmann::json::parse(cams_val.toString().toStdString());
                z.camera_ids = cams_j.get<std::vector<int>>();
            } catch(...) {}
        }
        
        z.max_occupancy = query.value(6).toInt();
        z.allow_loitering = query.value(7).toInt() != 0;
        z.loitering_threshold_sec = query.value(8).toInt();
        z.enable_alerts = query.value(9).toInt() != 0;
        z.max_alerts_per_minute = query.value(10).toInt();
        
        QVariant meta_val = query.value(11);
        if (!meta_val.isNull()) {
            try { z.metadata = nlohmann::json::parse(meta_val.toString().toStdString()); } catch(...) {}
        }
        
        z.created_at = std::chrono::system_clock::from_time_t(query.value(12).toLongLong());
        z.updated_at = std::chrono::system_clock::from_time_t(query.value(13).toLongLong());
        
        zones_[z.zone_id] = z;
    }
    
    LOG_INFO("[ZoneManager] Loaded {} zones", zones_.size());
    return true;
}

bool ZoneManager::saveToDatabase() {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_INFO("[ZoneManager] Saving zones to database...");
    
    auto& db_mgr = database::DbManager::getInstance();
    if (!db_mgr.isInitialized()) return false;
    
    QSqlDatabase db = db_mgr.getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_ERROR("[ZoneManager] Failed to get database connection");
        return false;
    }
    
    db_mgr.beginTransaction();
    
    // Clear existing zones
    {
        QSqlQuery del_query(db);
        if (!del_query.exec("DELETE FROM zones")) {
            LOG_ERROR("[ZoneManager] Failed to clear zones table: {}", del_query.lastError().text().toStdString());
            db_mgr.rollback();
            return false;
        }
    }
    
    // Prepare insert statement
    QSqlQuery query(db);
    if (!query.prepare("INSERT INTO zones (id, name, type, description, polygon_json, camera_ids_json, "
                       "max_occupancy, allow_loitering, loitering_threshold_sec, enable_alerts, "
                       "max_alerts_per_minute, metadata_json, created_at, updated_at) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")) {
        LOG_ERROR("[ZoneManager] Failed to prepare save query: {}", query.lastError().text().toStdString());
        db_mgr.rollback();
        return false;
    }
    
    for (const auto& [id, z] : zones_) {
        query.bindValue(0, z.zone_id);
        query.bindValue(1, QString::fromStdString(z.name));
        query.bindValue(2, QString::fromStdString(z.type));
        query.bindValue(3, QString::fromStdString(z.description));
        
        nlohmann::json poly_j = nlohmann::json::array();
        for (const auto& p : z.polygon) poly_j.push_back({{"x", p.x}, {"y", p.y}});
        query.bindValue(4, QString::fromStdString(poly_j.dump()));
        
        nlohmann::json cams_j = z.camera_ids;
        query.bindValue(5, QString::fromStdString(cams_j.dump()));
        
        query.bindValue(6, z.max_occupancy);
        query.bindValue(7, z.allow_loitering ? 1 : 0);
        query.bindValue(8, z.loitering_threshold_sec);
        query.bindValue(9, z.enable_alerts ? 1 : 0);
        query.bindValue(10, z.max_alerts_per_minute);
        
        query.bindValue(11, QString::fromStdString(z.metadata.dump()));
        
        query.bindValue(12, static_cast<qint64>(std::chrono::system_clock::to_time_t(z.created_at)));
        query.bindValue(13, static_cast<qint64>(std::chrono::system_clock::to_time_t(z.updated_at)));
        
        if (!query.exec()) {
            LOG_ERROR("[ZoneManager] Failed to insert zone {}: {}", z.zone_id, query.lastError().text().toStdString());
        }
    }
    
    db_mgr.commit();
    return true;
}

} // namespace vms::events

