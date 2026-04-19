#pragma once

#include <map>
#include <string>
#include <mutex>
#include <chrono>
#include "database/camera_repository.h"

namespace vms {
namespace utils {

/**
 * @brief Thread-safe singleton cache for camera ID → name mapping.
 * 
 * Shared by event_controller, recording_controller, and any other
 * component that needs to resolve camera names without hitting the DB
 * on every request.  Refreshes automatically every 60 seconds.
 */
class CameraNameCache {
public:
    static CameraNameCache& getInstance() {
        static CameraNameCache instance;
        return instance;
    }

    /**
     * @brief Get a snapshot of the camera name map (thread-safe copy).
     * Refreshes from DB if the cache is older than 60 seconds.
     */
    std::map<int, std::string> getNames() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        if (cache_.empty() ||
            std::chrono::duration_cast<std::chrono::seconds>(now - last_refresh_).count() > 60) {
            refresh();
            last_refresh_ = now;
        }
        return cache_;
    }

    /**
     * @brief Resolve a single camera name. Returns "Camera N" if not found.
     */
    std::string getName(int camera_id) {
        auto names = getNames();
        auto it = names.find(camera_id);
        return (it != names.end()) ? it->second : "Camera " + std::to_string(camera_id);
    }

private:
    CameraNameCache() = default;

    void refresh() {
        vms::database::CameraRepository repo;
        auto cameras = repo.getAllCameras();
        cache_.clear();
        for (const auto& c : cameras) {
            cache_[c.id] = c.name;
        }
    }

    std::map<int, std::string> cache_;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point last_refresh_{};
};

} // namespace utils
} // namespace vms
