#include "core/brands/hikvision_core.hpp"
#include "core/brands/HikvisionAdapter.hpp"
#include <pugixml.hpp>
#include <ctime>
#include <vector>
#include <string>

namespace vms {
namespace core {
namespace brands {

using json = nlohmann::json;

bool HikvisionCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    
    vms::HikvisionAdapter adapter(vcfg);
    return adapter.connect();
}

CameraDiscovery::DeviceInfo HikvisionCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vcfg.rtspPort = cfg.rtsp_port;

    vms::HikvisionAdapter adapter(vcfg);
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Hikvision;

    if (!adapter.connect()) {
        dev.error = "Kết nối Hikvision thất bại (ISAPI unreachable or auth error)";
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

std::vector<CameraDiscovery::CameraChannel> HikvisionCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::HikvisionAdapter adapter(vcfg);
    
    if (!adapter.connect()) return cameras;

    std::string body = adapter.rawGet("/ISAPI/Streaming/channels");
    if (body.empty()) return cameras;

    pugi::xml_document ch_doc;
    if (!ch_doc.load_string(body.c_str())) return cameras;

    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    for (auto& node : ch_doc.child("StreamingChannelList").children("StreamingChannel")) {
        std::string id_str = node.child_value("id");
        if (id_str.empty()) continue;
        int id = std::stoi(id_str);

        if (id % 100 != 1) continue;
        int ch_num = id / 100;

        CameraDiscovery::CameraChannel cam;
        cam.channel_id = ch_num;
        cam.name       = node.child_value("channelName");
        if (cam.name.empty()) cam.name = "Camera " + std::to_string(ch_num);
        cam.online = std::string(node.child_value("enabled")) == "true";

        auto vid = node.child("Video");
        if (vid) {
            std::string w = vid.child_value("videoResolutionWidth");
            std::string h = vid.child_value("videoResolutionHeight");
            if (!w.empty() && !h.empty()) cam.resolution = w + "x" + h;
        }

        cam.rtsp_main = base + "/Streaming/Channels/" + std::to_string(ch_num * 100 + 1);
        cam.rtsp_sub  = base + "/Streaming/Channels/" + std::to_string(ch_num * 100 + 2);
        cameras.push_back(cam);
    }
    
    return cameras;
}

json HikvisionCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::HikvisionAdapter adapter(vcfg);
    
    json specialized = json::object();
    if (adapter.connect()) {
        specialized["vca_raw"] = adapter.rawGet("/ISAPI/VCA/channels/1/capabilities");
        specialized["thermal_raw"] = adapter.rawGet("/ISAPI/Thermal/channels/1/thermometry");
        specialized["events_raw"] = adapter.rawGet("/ISAPI/Event/channels/1/capabilities");
    }
    return specialized;
}

bool HikvisionCore::ptzControl(const CameraDiscovery::DiscoveryConfig& cfg, const std::string& action, double speed) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::HikvisionAdapter adapter(vcfg);
    
    if (!adapter.connect()) return false;

    int spd = static_cast<int>(speed * 100);
    if (spd < 1) spd = 10;
    if (spd > 100) spd = 100;

    int pan = 0, tilt = 0, zoom = 0;
    if (action == "left") pan = -spd;
    else if (action == "right") pan = spd;
    else if (action == "up") tilt = spd;
    else if (action == "down") tilt = -spd;
    else if (action == "zoom_in") zoom = spd;
    else if (action == "zoom_out") zoom = -spd;

    std::string xml = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<PTZData version=\"2.0\" xmlns=\"http://www.isapi.org/ver20/XMLSchema\">\n"
        "  <pan>" + std::to_string(pan) + "</pan>\n"
        "  <tilt>" + std::to_string(tilt) + "</tilt>\n"
        "  <zoom>" + std::to_string(zoom) + "</zoom>\n"
        "</PTZData>";

    std::string out;
    return adapter.rawPut("/ISAPI/PTZCtrl/channels/1/continuous", xml) != "";
}

void HikvisionCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vcfg.rtspPort = cfg.rtsp_port;
    
    auto adapter = std::make_shared<vms::HikvisionAdapter>(vcfg);
    if (!adapter->connect()) return;

    adapter->startEventSubscription([onEvent](const vms::CameraEvent& ev) {
        // Normalize Hikvision-internal enum to the brand-agnostic string
        // vocabulary the consumer (camera_event_service_qt.cpp) routes on.
        // Pre-fix this emitted (int)ev.type, which made the consumer's
        // string-keyed routing silently ignore Hikvision hardware events.
        const char* evt_type = "hardware_alarm";
        switch (ev.type) {
            case vms::CameraEvent::Type::Motion:           evt_type = "motion_detect"; break;
            case vms::CameraEvent::Type::LineCrossing:     evt_type = "line_crossing"; break;
            case vms::CameraEvent::Type::Intrusion:        evt_type = "intrusion";     break;
            case vms::CameraEvent::Type::FaceDetection:    evt_type = "face";          break;
            case vms::CameraEvent::Type::TamperDetection:  evt_type = "tampering";     break;
            default:                                       evt_type = "hardware_alarm"; break;
        }

        nlohmann::json j = {
            {"type",         "camera_event"},
            {"brand",        "Hikvision"},
            {"event_type",   evt_type},
            {"active",       ev.active},
            {"channel",      ev.channel},
            {"native_code",  ev.typeRaw},
            {"extra",        ev.extra},
            {"timestamp",    (long long)std::time(nullptr)}
        };

        onEvent(j.dump());
    }, nullptr);

    // Keep it alive as long as needed - in a real system this would be managed by a subscription manager
    // For this POC, we'll let the caller manage the lifecycle if possible, or use a static map.
    // However, the current signature implies a synchronous-style call that starts a thread.
}

} // namespace brands
} // namespace core
} // namespace vms
