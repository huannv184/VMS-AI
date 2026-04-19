#include "core/camera_manager.h"
#include "core/camera_pipeline_manager.h"
#include "core/runtime_state.h"
#include "utils/logger.h"
#include "utils/validator.h"
#include <algorithm>
#include <vector>
#include <thread>
#include "core/brands/core_factory.hpp"
#include "core/camera_discovery.hpp"
#include "core/camera_event_service.h"

namespace vms {
namespace core {

CameraManager& CameraManager::getInstance() {
    static CameraManager instance;
    return instance;
}

bool CameraManager::init() {
    LOG_INFO("Initializing CameraManager...");
    
    camera_repo_ = std::make_unique<database::CameraRepository>();
    refreshCameraCache();
    ensureWorkersStarted();
    
    LOG_INFO("CameraManager initialized");
    
    // Auto-start active cameras in parallel
    auto cameras = getAllCameras();
    for (const auto& cam : cameras) {
        if (cam.is_active) {
            LOG_INFO("Scheduling auto-start for camera: {} (ID: {})", cam.name, cam.id);
            queueStartCamera(cam.id);
        }
    }
    
    LOG_INFO("Auto-start scheduled for active cameras");
    
    // Khởi động Health Supervisor để tự động kết nối lại
    stop_supervisor_.store(false, std::memory_order_release);
    supervisor_thread_ = std::thread(&CameraManager::healthSupervisor, this);
    
    return true;
}

void CameraManager::ensureWorkersStarted() {
    if (!start_worker_thread_.joinable()) {
        stop_start_worker_.store(false, std::memory_order_release);
        start_worker_thread_ = std::thread(&CameraManager::startWorkerLoop, this);
    }
}

void CameraManager::queueStartCamera(int camera_id) {
    if (camera_id <= 0 ||
        stop_start_worker_.load(std::memory_order_acquire) ||
        vms::core::shutting_down.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(start_queue_mutex_);
    if (!queued_camera_ids_.insert(camera_id).second) {
        return;
    }

    pending_start_queue_.push_back(camera_id);
    start_queue_cv_.notify_one();
}

void CameraManager::startWorkerLoop() {
    while (!stop_start_worker_.load(std::memory_order_acquire)) {
        int camera_id = -1;
        {
            std::unique_lock<std::mutex> lock(start_queue_mutex_);
            start_queue_cv_.wait(lock, [this]() {
                return stop_start_worker_.load(std::memory_order_acquire) ||
                       !pending_start_queue_.empty();
            });

            if (stop_start_worker_.load(std::memory_order_acquire)) {
                break;
            }

            camera_id = pending_start_queue_.front();
            pending_start_queue_.pop_front();
            queued_camera_ids_.erase(camera_id);
        }

        try {
            startCamera(camera_id);
        } catch (const std::exception& e) {
            LOG_ERROR("CameraManager start worker failed for camera {}: {}", camera_id, e.what());
        } catch (...) {
            LOG_ERROR("CameraManager start worker failed for camera {}: unknown exception", camera_id);
        }
    }
}

void CameraManager::healthSupervisor() {
    LOG_INFO("Health Supervisor started");
    while (!stop_supervisor_.load(std::memory_order_acquire) &&
           !vms::core::shutting_down.load(std::memory_order_acquire)) {
        // Chờ 60 giây (chia nhỏ để có thể ngắt sớm)
        for (int i = 0; i < 60 &&
                        !stop_supervisor_.load(std::memory_order_acquire) &&
                        !vms::core::shutting_down.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (stop_supervisor_.load(std::memory_order_acquire) ||
            vms::core::shutting_down.load(std::memory_order_acquire)) {
            break;
        }

        LOG_DEBUG("[WATCHDOG] Health Supervisor: Checking camera states...");
        auto& pipeline_mgr = CameraPipelineManager::getInstance();
        auto cameras = getAllCameras();

        for (const auto& cam : cameras) {
            if (!cam.is_active) continue;
            if (pipeline_mgr.isRunning(cam.id)) continue;

            // CRITICAL: Never restart a camera that permanently FAILED (exhausted all
            // reconnect attempts). That camera requires explicit operator intervention
            // (e.g., re-enable via API). Restarting here would undo the backoff and
            // cause an infinite restart storm at 60-second intervals.
            if (pipeline_mgr.getCameraState(cam.id) == CameraState::FAILED) {
                LOG_THROTTLED_WARN(300000,
                    "[WATCHDOG] Camera '{}' (ID: {}) is FAILED — supervisor restart suppressed. "
                    "Operator must re-enable the camera to resume streaming.",
                    cam.name, cam.id);
                continue;
            }

            LOG_INFO("[WATCHDOG] Camera '{}' (ID: {}) is active but not running — scheduling restart.",
                     cam.name, cam.id);
            queueStartCamera(cam.id);
        }
    }
    LOG_INFO("Health Supervisor stopped");
}

bool CameraManager::addCamera(Camera& camera) {
    LOG_INFO("Adding camera: {}", camera.name);
    
    if (!camera_repo_) {
        LOG_ERROR("CameraManager not initialized");
        return false;
    }
    
    bool result = camera_repo_->insertCamera(camera);
    
    if (result) {
        refreshCameraCache();
        LOG_INFO("Camera added successfully: {}", camera.name);
        // Auto-start if active
        if (camera.is_active) {
            startCamera(camera.id);
        }
    } else {
        LOG_ERROR("Failed to add camera: {}", camera.name);
    }
    
    return result;
}

bool CameraManager::removeCamera(int camera_id) {
    LOG_INFO("Removing camera ID: {}", camera_id);
    
    if (!camera_repo_) {
        LOG_ERROR("CameraManager not initialized");
        return false;
    }
    
    // CRITICAL FIX: Stop the pipeline FIRST while the database record still exists.
    // This prevents background threads from hitting "Camera not found" errors
    // during their shutdown phase.
    try {
        stopCamera(camera_id);
    } catch (const std::exception& e) {
        LOG_ERROR("Error stopping camera {} before removal: {}", camera_id, e.what());
    } catch (...) {
        LOG_ERROR("Unknown error stopping camera {} before removal", camera_id);
    }

    bool result = camera_repo_->deleteCamera(camera_id);
    
    if (result) {
        removeCameraCacheEntry(camera_id);
        LOG_INFO("Camera removed from Database: ID {}", camera_id);
        
        // Clear cached metadata
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        latest_metadata_.erase(camera_id);
        
        LOG_INFO("Camera cleanup complete: ID {}", camera_id);
    } else {
        LOG_ERROR("Failed to remove camera from Database: ID {}", camera_id);
    }
    
    return result;
}

bool CameraManager::startCamera(int camera_id) {
    if (vms::core::shutting_down.load(std::memory_order_acquire)) {
        LOG_WARN("Skipping start for camera {} during shutdown", camera_id);
        return false;
    }

    LOG_INFO("Starting camera ID: {}", camera_id);
    
    if (!camera_repo_) {
        LOG_ERROR("CameraManager not initialized");
        return false;
    }
    
    auto camera_opt = getCamera(camera_id);
    if (!camera_opt) {
        LOG_ERROR("Camera not found: ID {}", camera_id);
        return false;
    }
    
    auto camera = camera_opt.value();
    auto normalized_rtsp = vms::Validator::normalizeRtspUrl(camera.rtsp_url);
    if (!normalized_rtsp.has_value()) {
        LOG_ERROR("Camera {} has invalid RTSP URL", camera_id);
        return false;
    }

    auto& pipeline_manager = CameraPipelineManager::getInstance();
    if (pipeline_manager.isRunning(camera.id)) {
        LOG_INFO("Camera pipeline already running: ID {}", camera_id);
        return true;
    }
    
    // Monolith Mode: Start Pipeline directly
    bool pipeline_started = pipeline_manager.startPipeline(camera.id, normalized_rtsp.value());
    if (!pipeline_started) {
        LOG_ERROR("Failed to start pipeline for camera {}", camera_id);
        return false;
    }
    
    // Start Alarm Event Polling
    CameraEventService::getInstance().startListening(std::to_string(camera_id));
    
    // Update camera status
    camera.is_active = true;
    camera_repo_->updateCamera(camera);
    upsertCameraCache(camera);
    
    LOG_INFO("Camera pipeline started: ID {}", camera_id);
    return true;
}

bool CameraManager::stopCamera(int camera_id, bool update_db) {
    LOG_INFO("Stopping camera ID: {}", camera_id);
    
    if (!camera_repo_) {
        LOG_ERROR("CameraManager not initialized");
        return false;
    }
    
    // Monolith Mode: Stop Pipeline directly
    CameraPipelineManager::getInstance().stopPipeline(camera_id);
    
    // Stop Alarm Event Polling
    CameraEventService::getInstance().stopListening(std::to_string(camera_id));
    
    auto camera_opt = getCamera(camera_id);
    if (camera_opt) {
        auto camera = camera_opt.value();
        if (update_db) {
            camera.is_active = false;
            camera_repo_->updateCamera(camera);
        }
        upsertCameraCache(camera);
    }
    
    LOG_INFO("Camera pipeline stopped: ID {}", camera_id);
    return true;
}

std::vector<Camera> CameraManager::getAllCameras() {
    if (!camera_repo_) {
        LOG_ERROR("CameraManager not initialized");
        return {};
    }

    std::shared_lock<std::shared_mutex> lock(cameras_cache_mutex_);
    return cameras_cache_;
}

std::optional<Camera> CameraManager::getCamera(int camera_id) {
    if (!camera_repo_) {
        LOG_ERROR("CameraManager not initialized");
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> lock(cameras_cache_mutex_);
    auto it = cameras_by_id_cache_.find(camera_id);
    if (it == cameras_by_id_cache_.end()) {
        return std::nullopt;
    }

    return it->second;
}

bool CameraManager::updateCamera(const Camera& camera) {
    LOG_INFO("Updating camera: {} (ID: {})", camera.name, camera.id);
    
    if (!camera_repo_) {
        LOG_ERROR("CameraManager not initialized");
        return false;
    }
    
    bool result = camera_repo_->updateCamera(camera);
    
    if (result) {
        upsertCameraCache(camera);
        LOG_INFO("Camera updated successfully: {}", camera.name);
    } else {
        LOG_ERROR("Failed to update camera: {}", camera.name);
    }
    
    return result;
}

std::optional<CameraMetadata> CameraManager::getLatestMetadata(int camera_id) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    
    auto it = latest_metadata_.find(camera_id);
    if (it != latest_metadata_.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

void CameraManager::updateMetadata(int camera_id, const CameraMetadata& metadata) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    latest_metadata_[camera_id] = metadata;
}

bool CameraManager::refreshAdvancedConfig(int camera_id) {
    LOG_INFO("Refreshing advanced configuration for camera ID: {}", camera_id);
    
    auto camera_opt = getCamera(camera_id);
    if (!camera_opt) {
        LOG_ERROR("Camera not found: ID {}", camera_id);
        return false;
    }
    
    auto& camera = camera_opt.value();
    
    // Create discovery config from camera details
    CameraDiscovery::DiscoveryConfig cfg;
    cfg.host = "";
    
    // Simple RTSP URL parsing: rtsp://user:pass@host:port/path
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
    
    if (cfg.host.empty()) {
        LOG_ERROR("Could not parse host from RTSP URL: {}", camera.rtsp_url);
        return false;
    }

    // Try to determine brand from name or description if not explicitly stored
    auto detect_brand = [](const std::string& s) {
        std::string lower = s;
        for (auto& c : lower) c = tolower(c);
        // Hikvision family (OEM: Ezviz, Annke, LTS, etc.)
        if (lower.find("hik") != std::string::npos || lower.find("hikvision") != std::string::npos) return CameraDiscovery::Brand::Hikvision;
        if (lower.find("ezviz") != std::string::npos) return CameraDiscovery::Brand::Ezviz;
        // Dahua family (OEM: KBVision, Imou, CP Plus, Amcrest, etc.)
        if (lower.find("dahua") != std::string::npos) return CameraDiscovery::Brand::Dahua;
        if (lower.find("kbvision") != std::string::npos || lower.find("kb vision") != std::string::npos || lower.find("kbv") != std::string::npos) return CameraDiscovery::Brand::Kbvision;
        if (lower.find("imou") != std::string::npos) return CameraDiscovery::Brand::Imou;
        if (lower.find("vantech") != std::string::npos) return CameraDiscovery::Brand::Vantech;
        if (lower.find("questek") != std::string::npos) return CameraDiscovery::Brand::Questek;
        if (lower.find("cp plus") != std::string::npos || lower.find("cpplus") != std::string::npos) return CameraDiscovery::Brand::CPPlus;
        if (lower.find("amcrest") != std::string::npos) return CameraDiscovery::Brand::Amcrest;
        if (lower.find("lorex") != std::string::npos) return CameraDiscovery::Brand::Lorex;
        // Others
        if (lower.find("hanwha") != std::string::npos || lower.find("samsung") != std::string::npos) return CameraDiscovery::Brand::Hanwha;
        if (lower.find("axis") != std::string::npos) return CameraDiscovery::Brand::Axis;
        if (lower.find("bosch") != std::string::npos) return CameraDiscovery::Brand::Bosch;
        if (lower.find("uniview") != std::string::npos || lower.find("unv") != std::string::npos) return CameraDiscovery::Brand::Uniview;
        if (lower.find("pelco") != std::string::npos) return CameraDiscovery::Brand::Pelco;
        if (lower.find("reolink") != std::string::npos) return CameraDiscovery::Brand::Reolink;
        if (lower.find("milesight") != std::string::npos) return CameraDiscovery::Brand::Milesight;
        return CameraDiscovery::Brand::ONVIF;
    };
    
    cfg.brand = detect_brand(camera.name + " " + camera.description);
    
    auto core = vms::core::brands::CoreFactory::getCore(cfg.brand);
    if (!core) {
        LOG_WARN("No specialized core for brand: {}. Using generic ONVIF.", (int)cfg.brand);
        core = vms::core::brands::CoreFactory::getCore(CameraDiscovery::Brand::ONVIF);
    }
    
    if (core) {
        try {
            json advanced = core->getSpecializedConfig(cfg);
            camera.advanced_config = advanced.dump();
            camera_repo_->updateCamera(camera);
            upsertCameraCache(camera);
            LOG_INFO("Advanced config refreshed for camera ID: {}", camera_id);
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("Error refreshing advanced config: {}", e.what());
        }
    }
    
    return false;
}

void CameraManager::cleanup() {
    LOG_INFO("Cleaning up CameraManager...");
    stopAll();
    LOG_INFO("CameraManager cleanup complete");
}

void CameraManager::stopAll() {
    stop_supervisor_.store(true, std::memory_order_release);
    stop_start_worker_.store(true, std::memory_order_release);
    start_queue_cv_.notify_all();

    if (supervisor_thread_.joinable()) {
        supervisor_thread_.join();
    }

    if (start_worker_thread_.joinable()) {
        start_worker_thread_.join();
    }

    std::vector<int> active_camera_ids;
    if (camera_repo_) {
        auto cameras = getAllCameras();
        for (const auto& cam : cameras) {
            if (cam.is_active) {
                active_camera_ids.push_back(cam.id);
            }
        }
    }

    for (int camera_id : active_camera_ids) {
        stopCamera(camera_id, false);
    }

    CameraEventService::getInstance().stopAll();
}

void CameraManager::onDatabaseReady() {
    if (!camera_repo_) {
        camera_repo_ = std::make_unique<database::CameraRepository>();
    }

    refreshCameraCache();
    ensureWorkersStarted();

    auto cameras = getAllCameras();
    for (const auto& cam : cameras) {
        if (cam.is_active) {
            queueStartCamera(cam.id);
        }
    }
}

void CameraManager::refreshCameraCache() {
    if (!camera_repo_) {
        return;
    }

    auto cameras = camera_repo_->getAllCameras();

    std::unique_lock<std::shared_mutex> lock(cameras_cache_mutex_);
    cameras_cache_ = std::move(cameras);
    cameras_by_id_cache_.clear();
    for (const auto& camera : cameras_cache_) {
        cameras_by_id_cache_[camera.id] = camera;
    }
}

void CameraManager::upsertCameraCache(const Camera& camera) {
    std::unique_lock<std::shared_mutex> lock(cameras_cache_mutex_);
    cameras_by_id_cache_[camera.id] = camera;

    auto it = std::find_if(cameras_cache_.begin(), cameras_cache_.end(),
                           [camera_id = camera.id](const Camera& cached) {
                               return cached.id == camera_id;
                           });
    if (it != cameras_cache_.end()) {
        *it = camera;
        return;
    }

    cameras_cache_.push_back(camera);
    std::sort(cameras_cache_.begin(), cameras_cache_.end(),
              [](const Camera& lhs, const Camera& rhs) {
                  return lhs.id < rhs.id;
              });
}

void CameraManager::removeCameraCacheEntry(int camera_id) {
    std::unique_lock<std::shared_mutex> lock(cameras_cache_mutex_);
    cameras_by_id_cache_.erase(camera_id);
    cameras_cache_.erase(
        std::remove_if(cameras_cache_.begin(), cameras_cache_.end(),
                       [camera_id](const Camera& camera) {
                           return camera.id == camera_id;
                       }),
        cameras_cache_.end());
}

} // namespace core
} // namespace vms
