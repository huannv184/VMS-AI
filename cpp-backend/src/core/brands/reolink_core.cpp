#include "core/brands/reolink_core.hpp"
#include "core/brands/http_client.h"
#include "utils/logger.h"

namespace vms {
namespace core {
namespace brands {

bool ReolinkCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    std::string body = R"([{"cmd":"Login","param":{"User":{"userName":")" +
                        cfg.username + R"(","password":")" + cfg.password + R"("}}}])";
    auto resp = http.post("/api.cgi?cmd=Login", body, false, "application/json");
    return resp.ok() && resp.body.find("Token") != std::string::npos;
}

CameraDiscovery::DeviceInfo ReolinkCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Reolink;
    CameraDiscovery::HttpClient http(cfg);

    // --- Login to get token ---
    std::string login_body = R"([{"cmd":"Login","param":{"User":{"userName":")" +
                              cfg.username + R"(","password":")" + cfg.password + R"("}}}])";
    auto login_resp = http.post("/api.cgi?cmd=Login", login_body,
                                false, "application/json");
    if (!login_resp.ok()) { dev.error = "Reolink login failed"; return dev; }

    std::string token;
    try {
        auto j = json::parse(login_resp.body);
        token = j[0]["value"]["Token"]["name"].get<std::string>();
    } catch (...) { dev.error = "Could not parse Reolink token"; return dev; }

    // --- Device Info ---
    std::string dev_body = R"([{"cmd":"GetDevInfo","action":0,"param":{}}])";
    auto dev_resp = http.post("/api.cgi?cmd=GetDevInfo&token=" + token,
                              dev_body, false, "application/json");
    if (dev_resp.ok()) {
        try {
            auto j = json::parse(dev_resp.body);
            auto& info = j[0]["value"]["DevInfo"];
            dev.model    = info.value("name",    "");
            dev.serial   = info.value("serial",  "");
            dev.firmware = info.value("firmVer", "");
        } catch (...) { LOG_DEBUG("ReolinkCore: Failed to parse DevInfo JSON"); }
    }

    dev.cameras = getOptimizedStreams(cfg);
    dev.channels = (int)dev.cameras.size();
    return dev;
}

std::vector<CameraDiscovery::CameraChannel> ReolinkCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);
    
    // Reolink cameras are typically single channel (1 camera)
    CameraDiscovery::CameraChannel cam;
    cam.channel_id = 1;
    cam.name       = "Reolink Camera";
    cam.online     = true;
    cam.rtsp_main  = base + "/h264Preview_01_main";
    cam.rtsp_sub   = base + "/h264Preview_01_sub";
    cameras.push_back(cam);
    
    return cameras;
}

json ReolinkCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    return json::object();
}

} // namespace brands
} // namespace core
} // namespace vms
