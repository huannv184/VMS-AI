#include "core/brands/dahua_core.hpp"
#include "core/brands/DahuaAdapter.hpp"
#include <map>
#include <vector>
#include <string>
#include <sstream>

namespace vms {
namespace core {
namespace brands {

using json = nlohmann::json;

bool DahuaCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    
    // Sử dụng DahuaAdapter mới để thực hiện probe (xử lý được Digest Auth)
    vms::DahuaAdapter adapter(vcfg);
    return adapter.connect();
}

CameraDiscovery::DeviceInfo DahuaCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vcfg.rtspPort = cfg.rtsp_port;

    vms::DahuaAdapter adapter(vcfg);
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Dahua;

    if (!adapter.connect()) {
        dev.error = "Xác thực Dahua thất bại (Vui lòng kiểm tra tài khoản/mật khẩu)";
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

std::vector<CameraDiscovery::CameraChannel> DahuaCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    // Chúng ta vẫn có thể dùng logic cũ để lấy danh sách channel qua CGI 
    // nhưng dùng HttpClient mới trong DahuaAdapter để đảm bảo xác thực thành công.
    std::vector<CameraDiscovery::CameraChannel> cameras;
    
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::DahuaAdapter adapter(vcfg);
    
    if (!adapter.connect()) return cameras;

    // Lấy danh sách channel bằng cách gửi request thủ công qua adapter/http
    std::string body = adapter.rawGet("/cgi-bin/devVideoInput.cgi?action=getCollect");
    if (body.empty()) return cameras;

    auto lines = splitLines(body);
    std::map<int, CameraDiscovery::CameraChannel> ch_map;

    for (auto& line : lines) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        auto idx_s = key.find('[');
        auto idx_e = key.find(']');
        if (idx_s == std::string::npos) continue;
        int idx = std::stoi(key.substr(idx_s + 1, idx_e - idx_s - 1));
        std::string field = key.substr(idx_e + 2);

        auto& cam = ch_map[idx];
        if (field == "Name")    cam.name = val;
        if (field == "Channel") cam.channel_id = std::stoi(val) + 1;
    }

    if (ch_map.empty()) {
        CameraDiscovery::CameraChannel default_cam;
        default_cam.channel_id = 1;
        default_cam.name = "Dahua IPC 1";
        ch_map[1] = default_cam;
    }

    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    for (auto& [idx, cam] : ch_map) {
        if (cam.name.empty()) cam.name = "Dahua Camera " + std::to_string(cam.channel_id);
        int ch = cam.channel_id;
        cam.online = true;
        cam.rtsp_main = base + "/cam/realmonitor?channel=" + std::to_string(ch) + "&subtype=0";
        cam.rtsp_sub  = base + "/cam/realmonitor?channel=" + std::to_string(ch) + "&subtype=1";
        cameras.push_back(cam);
    }
    
    return cameras;
}

json DahuaCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::DahuaAdapter adapter(vcfg);
    
    json specialized = json::object();
    if (adapter.connect()) {
        std::string resp = adapter.rawGet("/cgi-bin/configManager.cgi?action=getConfig&name=VideoAnalyseRule");
        if (!resp.empty()) {
            specialized["analyse_rules"] = "Supported (IVS/Face)";
        }
    }
    return specialized;
}

std::string DahuaCore::extractCgi(const std::string& body, const std::string& key) {
    auto pos = body.find(key + "=");
    if (pos == std::string::npos) return "";
    pos += key.size() + 1;
    auto end = body.find('\n', pos);
    std::string val = body.substr(pos, end - pos);
    if (!val.empty() && val.back() == '\r') val.pop_back();
    return val;
}

std::vector<std::string> DahuaCore::splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

bool DahuaCore::ptzControl(const CameraDiscovery::DiscoveryConfig& cfg, const std::string& action, double speed) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::DahuaAdapter adapter(vcfg);
    
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

void DahuaCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    vms::CameraConfig vcfg;
    vcfg.ip = cfg.host;
    vcfg.username = cfg.username;
    vcfg.password = cfg.password;
    vcfg.httpPort = cfg.http_port;
    vms::DahuaAdapter adapter(vcfg);
    
    if (!adapter.connect()) return;

    std::atomic<bool> isRunning{true};
    adapter.startEventSubscription([&](const vms::CameraEvent& ev) {
        std::string typeMap = "EventNotificationAlert"; 
        if (ev.typeRaw == "VideoMotion") typeMap = "VMD";
        else if (ev.typeRaw == "CrossLineDetection") typeMap = "linedetection";
        else if (ev.typeRaw == "CrossRegionDetection") typeMap = "fielddetection";

        // Size > 50 and EventNotificationAlert required by camera_event_service.cpp
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
