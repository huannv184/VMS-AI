#include "core/brands/pelco_core.hpp"
#include "core/brands/http_client.h"

namespace vms {
namespace core {
namespace brands {

bool PelcoCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    // Pelco Sarix System API
    auto resp = http.get("/api/system/network/device");
    return resp.ok() && resp.body.find("model") != std::string::npos;
}

CameraDiscovery::DeviceInfo PelcoCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Pelco;
    CameraDiscovery::HttpClient http(cfg);

    auto resp = http.get("/api/system/network/device");
    if (!resp.ok()) { dev.error = "Cannot reach Pelco Sarix API"; return dev; }

    try {
        auto j       = json::parse(resp.body);
        dev.model    = j.value("model",        "");
        dev.serial   = j.value("serialNumber", "");
        dev.firmware = j.value("firmware",     "");
    } catch (...) { dev.error = "Pelco JSON parse error"; return dev; }

    dev.cameras = getOptimizedStreams(cfg);
    dev.channels = (int)dev.cameras.size();
    return dev;
}

std::vector<CameraDiscovery::CameraChannel> PelcoCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    CameraDiscovery::CameraChannel pelco_cam;
    pelco_cam.channel_id = 1;
    pelco_cam.name       = "Pelco Sarix 4K Optimized";
    pelco_cam.online     = true;
    
    // Direct Sarix Stream Endpoints (more stable than ONVIF for 4K)
    pelco_cam.rtsp_main  = base + "/stream1"; // Typically 4K/Main
    pelco_cam.rtsp_sub   = base + "/stream2"; // Typically Low-res
    
    cameras.push_back(pelco_cam);
    return cameras;
}

json vms::core::brands::PelcoCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    json specialized = json::object();
    
    // Pelco Sarix Advanced Info (Raw JSON)
    auto imaging_resp = http.get("/api/imaging/profiles", true);
    if (imaging_resp.ok()) specialized["imaging_raw"] = imaging_resp.body;
    
    auto perf_resp = http.get("/api/system/performance", true);
    if (perf_resp.ok()) specialized["performance_raw"] = perf_resp.body;

    auto events_resp = http.get("/api/events/active", true);
    if (events_resp.ok()) specialized["events_raw"] = events_resp.body;
    
    return specialized;
}

} // namespace brands
} // namespace core
} // namespace vms
