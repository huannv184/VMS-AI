#include "core/brands/hanwha_core.hpp"
#include "core/brands/HanwhaAdapter.hpp"
#include "utils/logger.h"
#include <vector>
#include <string>

namespace vms {
namespace core {
namespace brands {

using json = nlohmann::json;

bool HanwhaCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    
    vms::HanwhaAdapter adapter(vcfg);
    return adapter.connect();
}

CameraDiscovery::DeviceInfo HanwhaCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vcfg.rtspPort = cfg.rtsp_port;

    vms::HanwhaAdapter adapter(vcfg);
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Hanwha;

    if (!adapter.connect()) {
        dev.error = "Kết nối Hanwha thất bại (SUNAPI unreachable or auth error)";
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

std::vector<CameraDiscovery::CameraChannel> HanwhaCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    CameraDiscovery::CameraChannel cam;
    cam.channel_id = 1;
    cam.name       = "Main Camera (SUNAPI Default)";
    cam.online     = true;
    cam.rtsp_main  = base + "/profile1/media.smp";
    cam.rtsp_sub   = base + "/profile2/media.smp";
    cameras.push_back(cam);
    
    return cameras;
}

nlohmann::json HanwhaCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::HanwhaAdapter adapter(vcfg);
    
    json specialized = json::object();
    if (adapter.connect()) {
        specialized["thermal_raw"] = adapter.rawGet("/stw-cgi/thermal.cgi?msubmenu=thermalinfo&action=view");
        specialized["analytics_raw"] = adapter.rawGet("/stw-cgi/media.cgi?msubmenu=analytics&action=view");
        specialized["stabilization_raw"] = adapter.rawGet("/stw-cgi/media.cgi?msubmenu=streamstabilization&action=view");
    }
    return specialized;
}

bool HanwhaCore::ptzControl(const CameraDiscovery::DiscoveryConfig& cfg, const std::string& action, double speed) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::HanwhaAdapter adapter(vcfg);
    
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

void HanwhaCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::HanwhaAdapter adapter(vcfg);
    
    if (!adapter.connect()) return;

    std::atomic<bool> isRunning{true};
    adapter.startEventSubscription([&](const vms::CameraEvent& ev) {
        std::string typeMap = "EventNotificationAlert"; 
        if (ev.type == vms::CameraEvent::Type::Motion) typeMap = "VMD";
        else if (ev.type == vms::CameraEvent::Type::LineCrossing) typeMap = "linedetection";
        else if (ev.type == vms::CameraEvent::Type::Intrusion) typeMap = "fielddetection";

        std::string fakeXml = std::string(60, ' ') + "<EventNotificationAlert><eventType>" + typeMap + "</eventType></EventNotificationAlert>";
        if (!onEvent(fakeXml)) {
            isRunning = false;
        }
    }, nullptr);

    while (isRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!onEvent("keepalive")) {
            isRunning = false;
        }
    }
}

} // namespace brands
} // namespace core
} // namespace vms
