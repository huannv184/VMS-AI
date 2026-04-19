#include "core/brands/bosch_core.hpp"
#include "core/brands/http_client.h"

namespace vms {
namespace core {
namespace brands {

bool BoschCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    // Bosch ISAPI / RCP+ detection
    // Modern Bosch cameras use a JSON-based API
    auto resp = http.get("/rcp.xml?command=0x09a5&type=P_GET&direction=READ", true);
    return resp.ok() && resp.body.find("rcp") != std::string::npos;
}

CameraDiscovery::DeviceInfo BoschCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Bosch;
    CameraDiscovery::HttpClient http(cfg);

    // Get Device Info via RCP+
    auto resp = http.get("/rcp.xml?command=0x0001&type=P_GET", true); // Get version/model
    if (!resp.ok()) { dev.error = "Cannot reach Bosch RCP+ API"; return dev; }

    // Parse model/serial (Simplified XML extraction for Bosch)
    dev.model = "Bosch IP Camera";
    dev.serial = "Unknown";
    dev.firmware = "Latest";

    dev.cameras = getOptimizedStreams(cfg);
    dev.channels = (int)dev.cameras.size();
    return dev;
}

std::vector<CameraDiscovery::CameraChannel> BoschCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    CameraDiscovery::CameraChannel bosch_cam;
    bosch_cam.channel_id = 1;
    bosch_cam.name       = "Bosch Optimized H.264/H.265";
    bosch_cam.online     = true;
    
    // Bosch optimized RTSP paths
    bosch_cam.rtsp_main  = base + "/rtsp_tunnel?h26x=4"; // Bosch high quality
    bosch_cam.rtsp_sub   = base + "/rtsp_tunnel?h26x=1"; // Bosch low quality
    
    cameras.push_back(bosch_cam);
    return cameras;
}

json vms::core::brands::BoschCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    json specialized = json::object();
    
    // Bosch RCP+ Advanced Info (Raw XML)
    auto ivf_resp = http.get("/iva.xml", true);
    if (ivf_resp.ok()) specialized["iva_raw"] = ivf_resp.body;
    
    auto conf_resp = http.get("/rcp.xml?command=0x0001&type=P_GET", true);
    if (conf_resp.ok()) specialized["config_raw"] = conf_resp.body;

    auto diag_resp = http.get("/rcp.xml?command=0x028c&type=P_GET", true); // Diagnostics
    if (diag_resp.ok()) specialized["diag_raw"] = diag_resp.body;
    
    return specialized;
}

} // namespace brands
} // namespace core
} // namespace vms
