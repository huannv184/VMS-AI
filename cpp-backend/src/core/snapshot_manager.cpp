#include "core/snapshot_manager.h"
#include "utils/logger.h"
#include <filesystem>
#include <fstream>
#include <ctime>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace vms {
namespace core {

SnapshotManager& SnapshotManager::getInstance() {
    static SnapshotManager instance;
    return instance;
}

std::string SnapshotManager::takeSnapshot(int camera_id) {
    // In a real system, this would call CameraPipelineManager to get a frame.
    // For this audit fix, we'll simulate a successful capture if we can't link to the pipeline.
    // We already have saveSnapshot for binary data.
    
    LOG_INFO("Taking snapshot for camera {}", camera_id);
    return ""; // Placeholder for direct pipeline capture logic
}

std::string SnapshotManager::saveSnapshot(int camera_id, const unsigned char* jpeg_data, size_t size, long long timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (timestamp == 0) {
        timestamp = static_cast<long long>(std::time(nullptr));
    }

    std::filesystem::create_directories(storage_path_);
    std::string filename = generateFilename(camera_id, timestamp);
    std::string full_path = storage_path_ + filename;

    std::ofstream out(full_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR("Failed to save snapshot file: {}", full_path);
        return "";
    }

    out.write(reinterpret_cast<const char*>(jpeg_data), size);
    out.close();

    Snapshot snap;
    snap.id = "snap_" + std::to_string(camera_id) + "_" + std::to_string(timestamp);
    snap.camera_id = camera_id;
    snap.trigger = "manual";
    snap.filepath = filename;
    snap.timestamp = timestamp;
    snap.timestamp_str = std::to_string(timestamp);
    snap.metadata_json = "{}";

    updateIndex(snap);
    
    LOG_INFO("Snapshot saved: {} for camera {}", full_path, camera_id);
    return filename;
}

std::vector<Snapshot> SnapshotManager::getRecentSnapshots(int camera_id, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshots_.empty()) {
        loadIndex();
    }

    std::vector<Snapshot> result;
    for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
        if (camera_id != -1 && it->camera_id != camera_id) continue;
        result.push_back(*it);
        if ((int)result.size() >= limit) break;
    }
    return result;
}

bool SnapshotManager::deleteSnapshot(const std::string& snapshot_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshots_.empty()) loadIndex();

    auto it = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const Snapshot& s) {
        return s.id == snapshot_id;
    });

    if (it != snapshots_.end()) {
        std::filesystem::remove(std::filesystem::path(storage_path_) / it->filepath);
        snapshots_.erase(it);
        
        // Update index file
        json index = json::array();
        for (const auto& s : snapshots_) {
            index.push_back({
                {"id", s.id},
                {"camera_id", s.camera_id},
                {"trigger", s.trigger},
                {"filepath", s.filepath},
                {"timestamp", s.timestamp_str},
                {"metadata", json::parse(s.metadata_json)}
            });
        }
        
        std::ofstream out(storage_path_ + "index.json");
        if (out.is_open()) {
            out << index.dump(2);
        }
        return true;
    }
    return false;
}

std::string SnapshotManager::generateFilename(int camera_id, long long timestamp) {
    return "snap_" + std::to_string(camera_id) + "_" + std::to_string(timestamp) + ".jpg";
}

void SnapshotManager::updateIndex(const Snapshot& snap) {
    snapshots_.push_back(snap);
    
    // Maintain a simple JSON index for the frontend to query
    json index = json::array();
    if (std::filesystem::exists(storage_path_ + "index.json")) {
        try {
            std::ifstream in(storage_path_ + "index.json");
            in >> index;
        } catch (...) {
            index = json::array();
        }
    }

    index.push_back({
        {"id", snap.id},
        {"camera_id", snap.camera_id},
        {"trigger", snap.trigger},
        {"filepath", snap.filepath},
        {"timestamp", snap.timestamp_str},
        {"metadata", json::parse(snap.metadata_json)}
    });

    std::ofstream out(storage_path_ + "index.json", std::ios::out | std::ios::trunc);
    if (out.is_open()) {
        out << index.dump(2);
    }
}

void SnapshotManager::loadIndex() {
    snapshots_.clear();
    std::string index_path = storage_path_ + "index.json";
    if (!std::filesystem::exists(index_path)) return;

    try {
        std::ifstream in(index_path);
        json index;
        in >> index;
        
        for (const auto& item : index) {
            Snapshot s;
            s.id = item.value("id", "");
            s.camera_id = item.value("camera_id", -1);
            s.trigger = item.value("trigger", "");
            s.filepath = item.value("filepath", "");
            s.timestamp_str = item.value("timestamp", "0");
            s.timestamp = std::stoll(s.timestamp_str);
            s.metadata_json = item.value("metadata", json::object()).dump();
            snapshots_.push_back(s);
        }
    } catch (...) {
        LOG_ERROR("Failed to load snapshot index");
    }
}

} // namespace core
} // namespace vms
