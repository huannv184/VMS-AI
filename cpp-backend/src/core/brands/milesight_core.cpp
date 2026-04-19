#include "core/brands/milesight_core.hpp"
#include "core/brands/http_client.h"
#include "utils/logger.h"

namespace vms {
namespace core {
namespace brands {

bool MilesightCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    auto resp = http.get("/api/v1/system/device_info");
    return resp.ok() && resp.body.find("model") != std::string::npos;
}

CameraDiscovery::DeviceInfo MilesightCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Milesight;
    CameraDiscovery::HttpClient http(cfg);

    auto resp = http.get("/api/v1/system/device_info");
    if (!resp.ok()) { dev.error = "Cannot reach Milesight API"; return dev; }

    try {
        auto j = json::parse(resp.body);
        dev.model    = j.value("model",            "");
        dev.serial   = j.value("serial_number",    "");
        dev.firmware = j.value("firmware_version", "");
    } catch (...) { dev.error = "Milesight JSON parse error"; return dev; }

    dev.cameras = getOptimizedStreams(cfg);
    dev.channels = (int)dev.cameras.size();
    return dev;
}

std::vector<CameraDiscovery::CameraChannel> MilesightCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    CameraDiscovery::HttpClient http(cfg);
    
    auto prof_resp = http.get("/api/v1/video/profiles");
    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    if (prof_resp.ok()) {
        try {
            auto j = json::parse(prof_resp.body);
            auto profiles = j["profiles"];
            CameraDiscovery::CameraChannel cam;
            cam.channel_id = 1;
            cam.name       = "Main Camera";
            cam.online     = true;
            for (auto& p : profiles) {
                int id = p.value("id", 0);
                if (id == 1) { // Main Stream
                    cam.resolution = p.value("resolution", "");
                    cam.codec      = p.value("codec",      "");
                    cam.rtsp_main  = base + "/main";
                } else if (id == 2) { // Sub Stream
                    cam.rtsp_sub = base + "/sub";
                }
            }
            cameras.push_back(cam);
        } catch (...) { LOG_DEBUG("MilesightCore: Failed to parse video profiles JSON"); }
    }

    if (cameras.empty()) {
        CameraDiscovery::CameraChannel cam;
        cam.channel_id = 1; cam.name = "Main Camera"; cam.online = true;
        cam.rtsp_main = base + "/main";
        cam.rtsp_sub  = base + "/sub";
        cameras.push_back(cam);
    }
    
    return cameras;
}

json MilesightCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    return json::object();
}

} // namespace brands
} // namespace core
} // namespace vms
