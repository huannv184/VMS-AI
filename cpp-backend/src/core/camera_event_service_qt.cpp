#include "core/camera_event_service.h"
#include "streaming/camera_stream_manager_qt.h"
#include "core/brands/core_factory.hpp"
#include "utils/logger.h"
#include "database/camera_repository.h"
#include "database/traffic_repository.h"
namespace vms {
namespace core {

CameraEventService& CameraEventService::getInstance() {
    static CameraEventService instance;
    return instance;
}

CameraEventService::~CameraEventService() {
    stopAll();
}

void CameraEventService::startListening(const std::string& cam_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(cam_id) != sessions_.end()) return;

    auto session = std::make_shared<EventSession>();
    session->cam_id = cam_id;
    session->worker = std::thread(&CameraEventService::workerLoop, this, session);
    sessions_[cam_id] = session;
    LOG_INFO("CameraEventService started listing for camera {}", cam_id);
}

void CameraEventService::stopListening(const std::string& cam_id) {
    std::shared_ptr<EventSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(cam_id);
        if (it == sessions_.end()) return;
        session = it->second;
        session->stop_flag = true;
        sessions_.erase(it);
    }
    // Join OUTSIDE the lock to prevent deadlock.
    if (session->worker.joinable()) {
        session->worker.join();
    }
    LOG_INFO("CameraEventService stopped listening for camera {}", cam_id);
}

void CameraEventService::stopAll() {
    std::vector<std::shared_ptr<EventSession>> all_sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : sessions_) {
            pair.second->stop_flag = true;
            all_sessions.push_back(pair.second);
        }
        sessions_.clear();
    }

    for (auto& session : all_sessions) {
        if (session->worker.joinable()) {
            session->worker.join();
        }
    }
}

CameraDiscovery::DiscoveryConfig CameraEventService::getCameraConfig(const std::string& cam_id) {
    CameraDiscovery::DiscoveryConfig cfg;
    try {
        int id = std::stoi(cam_id);
        
        database::CameraRepository repo;
        auto camera_opt = repo.getCameraById(id);
        if (camera_opt) {
            auto& camera = camera_opt.value();
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
            cfg.http_port = 80;

            std::string lower = camera.name + " " + camera.description;
            for (auto& c : lower) c = tolower(c);
            if (lower.find("hanwha") != std::string::npos || lower.find("samsung") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Hanwha;
            else if (lower.find("axis") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Axis;
            else if (lower.find("bosch") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Bosch;
            else if (lower.find("hik") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Hikvision;
            else if (lower.find("dahua") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Dahua;
            else if (lower.find("uniview") != std::string::npos || lower.find("unv") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Uniview;
            else if (lower.find("pelco") != std::string::npos) cfg.brand = CameraDiscovery::Brand::Pelco;
            else cfg.brand = CameraDiscovery::Brand::ONVIF;
        }
    } catch (...) {}
    return cfg;
}

bool CameraEventService::sendPTZCommand(const std::string& cam_id, const std::string& action, double speed) {
    CameraDiscovery::DiscoveryConfig cfg = getCameraConfig(cam_id);
    if (cfg.host.empty()) return false;
    
    auto core = brands::CoreFactory::getCore(cfg.brand);
    if (core) {
        LOG_INFO("CameraEventService PTZ {} to camera {} via ISAPI/CGI", action, cam_id);
        return core->ptzControl(cfg, action, speed);
    }
    return false;
}

void CameraEventService::workerLoop(std::shared_ptr<EventSession> session) {
    while (!session->stop_flag) {
        CameraDiscovery::DiscoveryConfig cfg = getCameraConfig(session->cam_id);
        if (cfg.host.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        
        auto core = brands::CoreFactory::getCore(cfg.brand);
        if (!core) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            continue;
        }

        LOG_INFO("CameraEventService polling events for {}", session->cam_id);
        
        // This is a blocking/chunk stream function
        core->pullEvents(cfg, [session](const std::string& chunk) -> bool {
            if (session->stop_flag) return false;
            
            // Only broadcast non-empty keep-alives or actual alarms
            if (chunk.size() > 50 && chunk.find("EventNotificationAlert") != std::string::npos) {
                // Determine event type
                std::string evt_type = "hardware_alarm";
                if (chunk.find("VMD") != std::string::npos) evt_type = "motion_detect";
                else if (chunk.find("linedetection") != std::string::npos) evt_type = "line_crossing";
                else if (chunk.find("fielddetection") != std::string::npos) evt_type = "intrusion";
                
                nlohmann::json notif = {
                    {"type", "event"},
                    {"camera_id", std::stoi(session->cam_id)},
                    {"event_type", evt_type},
                    {"severity", "major"},
                    {"message", "Phát hiện tín hiệu báo động phần cứng (Edge AI)"},
                    {"timestamp", std::time(nullptr)}
                };
                
                // 4. Persistence for Analytics (Counter)
                if (evt_type == "line_crossing" || evt_type == "intrusion") {
                    vms::TrafficCount tc;
                    tc.camera_id = std::stoi(session->cam_id);
                    tc.roi_id = 1; // Default ROI for now
                    tc.direction = (evt_type == "line_crossing") ? "in" : "out";
                    tc.vehicle_type = "person"; // Default
                    tc.count = 1;
                    tc.period_start = std::time(nullptr);
                    tc.period_end = tc.period_start;
                    
                    vms::database::TrafficRepository traffic_repo;
                    traffic_repo.insertCount(tc);
                }

                // Broadcast to Global Dashboard (Cam 0) and local camera stream
                vms::streaming::CameraStreamManager::getInstance().broadcastEvent(0, notif);
                vms::streaming::CameraStreamManager::getInstance().broadcastEvent(std::stoi(session->cam_id), notif);
                LOG_INFO("CameraEventService intercepted hardware alarm: {}", evt_type);
            }
            return !session->stop_flag;
        });

        // If Stream disconnects, wait 5 sec before reconnecting
        if (!session->stop_flag) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

} // namespace core
} // namespace vms
