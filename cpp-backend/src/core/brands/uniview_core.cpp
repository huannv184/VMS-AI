#include "core/brands/uniview_core.hpp"
#include "core/brands/http_client.h"
#include "utils/logger.h"

namespace vms {
namespace core {
namespace brands {

bool UniviewCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    auto resp = http.get("/LAPI/V1.0/System/DeviceBasicInfo");
    return resp.ok() && resp.body.find("SerialNumber") != std::string::npos;
}

CameraDiscovery::DeviceInfo UniviewCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Uniview;
    CameraDiscovery::HttpClient http(cfg);

    auto resp = http.get("/LAPI/V1.0/System/DeviceBasicInfo");
    if (!resp.ok()) { dev.error = "Cannot reach Uniview LAPI"; return dev; }

    try {
        auto j       = json::parse(resp.body);
        auto info    = j.value("Response", json::object()).value("Data", json::object());
        dev.model    = info.value("Model",        "");
        dev.serial   = info.value("SerialNumber", "");
        dev.firmware = info.value("SoftwareVersion", "");
    } catch (...) { dev.error = "Uniview LAPI JSON parse error"; return dev; }

    dev.cameras = getOptimizedStreams(cfg);
    dev.channels = (int)dev.cameras.size();
    return dev;
}

std::vector<CameraDiscovery::CameraChannel> UniviewCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    CameraDiscovery::HttpClient http(cfg);
    
    auto ch_resp = http.get("/LAPI/V1.0/Channels/VideoIn");
    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    if (ch_resp.ok()) {
        try {
            auto j = json::parse(ch_resp.body);
            auto data = j.value("Response", json::object()).value("Data", json::object());
            if (data.contains("VideoInItem")) {
                for (auto& item : data["VideoInItem"]) {
                    int ch = item.value("ID", 1);
                    CameraDiscovery::CameraChannel cam;
                    cam.channel_id = ch;
                    cam.name       = "Uniview Camera " + std::to_string(ch);
                    cam.online     = true;
                    cam.rtsp_main  = base + "/media/video" + std::to_string(ch);
                    cam.rtsp_sub   = base + "/media/video" + std::to_string(ch) + "/sub";
                    cameras.push_back(cam);
                }
            }
        } catch (...) { LOG_DEBUG("UniviewCore: Failed to parse channel list JSON"); }
    }

    if (cameras.empty()) {
        CameraDiscovery::CameraChannel cam;
        cam.channel_id = 1; cam.name = "Uniview Main Camera"; cam.online = true;
        cam.rtsp_main  = base + "/media/video1";
        cam.rtsp_sub   = base + "/media/video1/sub";
        cameras.push_back(cam);
    }
    
    return cameras;
}

nlohmann::json UniviewCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    return nlohmann::json::object();
}

void UniviewCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    CameraDiscovery::HttpClient http(cfg);
    // Uniview uses long-polling or HTTP multipart for VCA/Alarm
    // GET /LAPI/V1.0/System/Event/Subscription (HTTP Stream)
    
    std::string path = "/LAPI/V1.0/System/Event/Subscription";
    bool running = true;
    
    while (running) {
        http.streamGet(path, [onEvent](const std::string& chunk) -> bool {
            // Simplified: extract event types from Uniview LAPI message
            if (chunk.find("\"EventType\"") != std::string::npos) {
                  return onEvent(chunk);
            }
            return true; // Continue streaming
        });
        
        if (running) std::this_thread::sleep_for(std::chrono::seconds(5)); // Retry delay
    }
}

} // namespace brands
} // namespace core
} // namespace vms
