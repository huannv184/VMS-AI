#include "core/brands/axis_core.hpp"
#include "core/brands/AxisAdapter.hpp"
#include "utils/logger.h"
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

namespace vms {
namespace core {
namespace brands {

using json = nlohmann::json;

bool AxisCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    
    vms::AxisAdapter adapter(vcfg);
    return adapter.connect();
}

CameraDiscovery::DeviceInfo AxisCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vcfg.rtspPort = cfg.rtsp_port;

    vms::AxisAdapter adapter(vcfg);
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Axis;

    if (!adapter.connect()) {
        dev.error = "Kết nối Axis thất bại (VAPIX unreachable or auth error)";
        return dev;
    }

    auto info = adapter.getDeviceInfo();
    dev.model = info.model;
    dev.serial = info.serialNumber;
    dev.firmware = info.firmwareVersion;

    dev.cameras = getOptimizedStreams(cfg);
    dev.channels = (int)dev.cameras.size();
    return dev;
}

std::vector<CameraDiscovery::CameraChannel> AxisCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::AxisAdapter adapter(vcfg);
    
    if (!adapter.connect()) return cameras;

    // Fetch video source capabilities
    std::string params = adapter.rawGet("/axis-cgi/param.cgi?action=list&group=Image,Properties.API.HTTP.Version");
    
    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    CameraDiscovery::CameraChannel cam;
    cam.channel_id = 1;
    cam.name       = "Axis Camera 1";
    cam.online     = true;
    
    // Determine optimal resolution from params if possible
    if (params.find("Image.I0.Appearance.Resolution=1920x1080") != std::string::npos) {
        cam.resolution = "1920x1080";
    } else {
        cam.resolution = "1280x720";
    }

    cam.rtsp_main = base + "/axis-media/media.amp?videocodec=h264&resolution=" + cam.resolution;
    cam.rtsp_sub  = base + "/axis-media/media.amp?videocodec=h264&resolution=640x360";
    
    cameras.push_back(cam);
    return cameras;
}

json AxisCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::AxisAdapter adapter(vcfg);
    
    json specialized = json::object();
    if (adapter.connect()) {
        specialized["properties_raw"] = adapter.rawGet("/axis-cgi/param.cgi?action=list&group=Properties");
        specialized["video_source_raw"] = adapter.rawGet("/axis-cgi/param.cgi?action=list&group=Image,VideoSource");
        specialized["vmd_raw"] = adapter.rawGet("/axis-cgi/param.cgi?action=list&group=VMD");
    }
    return specialized;
}

bool AxisCore::ptzControl(const CameraDiscovery::DiscoveryConfig& cfg, const std::string& action, double speed) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::AxisAdapter adapter(vcfg);
    
    if (!adapter.connect()) return false;

    vms::PtzCommand cmd;
    cmd.panSpeed = speed;
    cmd.tiltSpeed = speed;
    cmd.zoomSpeed = speed;

    if (action == "up") cmd.move = vms::PtzCommand::Move::Up;
    else if (action == "down") cmd.move = vms::PtzCommand::Move::Down;
    else if (action == "left") cmd.move = vms::PtzCommand::Move::Left;
    else if (action == "right") cmd.move = vms::PtzCommand::Move::Right;
    else if (action == "zoom_in") cmd.move = vms::PtzCommand::Move::ZoomIn;
    else if (action == "zoom_out") cmd.move = vms::PtzCommand::Move::ZoomOut;
    else if (action == "stop") cmd.action = vms::PtzCommand::Action::Stop;
    else return false;

    return adapter.ptzControl(cmd);
}

// BUG-EVENTS-01 (2026-05-07): pre-fix, this called a no-op
// `AxisAdapter::startEventSubscription` and then spun a `keepalive` loop —
// real Axis VAPIX 3 event subscription was never wired. See ONVIFCore for
// the full lesson; same shape, same fix.
void AxisCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    LOG_WARN("[AxisCore] Hardware event subscription is NOT implemented "
             "for Axis host {}:{}. Real Axis events require VAPIX 3 event "
             "stream wiring. Sleeping 60s before returning.",
             cfg.host, cfg.http_port);
    (void)onEvent;
    std::this_thread::sleep_for(std::chrono::seconds(60));
}

} // namespace brands
} // namespace core
} // namespace vms
