#pragma once

#include "database/models.h"
#include "database/camera_repository.h"
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <shared_mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <optional>

namespace vms {
namespace core {

/**
 * @brief Camera manager for lifecycle management
 */
class CameraManager {
public:
    static CameraManager& getInstance();
    
    /**
     * @brief Initialize camera manager
     */
    bool init();
    
    /**
     * @brief Add new camera
     */
    bool addCamera(Camera& camera);
    
    /**
     * @brief Remove camera
     */
    bool removeCamera(int camera_id);
    
    /**
     * @brief Start camera processing
     */
    bool startCamera(int camera_id);
    
    /**
     * @brief Stop camera processing
     */
    bool stopCamera(int camera_id, bool update_db = true);
    
    /**
     * @brief Get all cameras
     */
    std::vector<Camera> getAllCameras();
    
    /**
     * @brief Get camera by ID
     */
    std::optional<Camera> getCamera(int camera_id);
    
    /**
     * @brief Update camera
     */
    bool updateCamera(const Camera& camera);

    database::CameraRepository& getRepository() { return *camera_repo_; }
    
    /**
     * @brief Refresh advanced configuration for a camera
     */
    bool refreshAdvancedConfig(int camera_id);
    
    /**
     * @brief Get latest metadata for camera
     */
    std::optional<CameraMetadata> getLatestMetadata(int camera_id);
    
    /**
     * @brief Update latest metadata for camera
     */
    void updateMetadata(int camera_id, const CameraMetadata& metadata);
    
    /**
     * @brief Cleanup all resources
     */
    void cleanup();

    void stopAll();
    void onDatabaseReady();

private:
    CameraManager() = default;
    ~CameraManager() = default;
    
    CameraManager(const CameraManager&) = delete;
    CameraManager& operator=(const CameraManager&) = delete;
    
    std::unique_ptr<database::CameraRepository> camera_repo_;
    std::map<int, CameraMetadata> latest_metadata_;
    std::mutex metadata_mutex_;

    // Health Supervisor: Triển khai cơ chế tự động kết nối lại camera bị ngắt
    std::thread supervisor_thread_;
    std::atomic<bool> stop_supervisor_{false};
    void healthSupervisor();

    std::thread start_worker_thread_;
    std::atomic<bool> stop_start_worker_{false};
    std::mutex start_queue_mutex_;
    std::condition_variable start_queue_cv_;
    std::deque<int> pending_start_queue_;
    std::unordered_set<int> queued_camera_ids_;

    void ensureWorkersStarted();
    void queueStartCamera(int camera_id);
    void startWorkerLoop();
    void refreshCameraCache();
    void upsertCameraCache(const Camera& camera);
    void removeCameraCacheEntry(int camera_id);

    mutable std::shared_mutex cameras_cache_mutex_;
    std::vector<Camera> cameras_cache_;
    std::unordered_map<int, Camera> cameras_by_id_cache_;
};

} // namespace core
} // namespace vms
