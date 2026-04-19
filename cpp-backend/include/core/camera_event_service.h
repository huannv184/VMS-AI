#pragma once
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include "database/db_manager.h"
#include "core/camera_types.h"

namespace vms {
namespace core {

class CameraEventService {
public:
    static CameraEventService& getInstance();
    
    // Start listening to hardware events via ISAPI/CGI
    void startListening(const std::string& cam_id);
    
    // Stop listening
    void stopListening(const std::string& cam_id);

    void stopAll();
    
    // Control PTZ
    bool sendPTZCommand(const std::string& cam_id, const std::string& action, double speed);

private:
    CameraEventService() = default;
    ~CameraEventService();
    
    // Prevent copy
    CameraEventService(const CameraEventService&) = delete;
    CameraEventService& operator=(const CameraEventService&) = delete;

    struct EventSession {
        std::string cam_id;
        std::atomic<bool> stop_flag{false};
        std::thread worker;
    };
    
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<EventSession>> sessions_;

    void workerLoop(std::shared_ptr<EventSession> session);
    
    CameraDiscovery::DiscoveryConfig getCameraConfig(const std::string& cam_id);
};

} // namespace core
} // namespace vms
