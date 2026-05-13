#include "core/brands/dahua_core.hpp"
#include "core/brands/DahuaAdapter.hpp"
#include "core/brands/http_client.h"
#include "utils/logger.h"
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>

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

// BUG-EVENTS-01 (2026-05-07): pre-fix this called a no-op
// `DahuaAdapter::startEventSubscription` and then spun a keepalive loop.
// Real Dahua EventManager subscription (HTTP multipart on
// /cgi-bin/eventManager.cgi?action=attach) was never wired. See ONVIFCore
// for the full lesson.
// Map Dahua native Code= identifiers to the brand-agnostic event_type vocabulary
// the consumer (camera_event_service_qt.cpp) routes on.
//
// Dahua emits dozens of codes; we map only the ones that have a one-to-one
// equivalent in the VMS event model. Unmapped codes fall back to
// "hardware_alarm" so operators still see something fire instead of a silent
// drop (BUG-EVENTS-01 lesson: silent-drop is worse than imprecise dispatch).
static std::string dahuaCodeToEventType(const std::string& code) {
    if (code == "VideoMotion")            return "motion_detect";
    if (code == "CrossLineDetection")     return "line_crossing";
    if (code == "CrossRegionDetection")   return "intrusion";
    if (code == "LoiteringDetection" ||
        code == "WanderDetection")        return "loitering";
    if (code == "FaceDetection")          return "face";
    if (code == "AudioAnomaly" ||
        code == "AudioMutation")          return "audio_alarm";
    if (code == "VideoBlind"  ||
        code == "VideoLoss"   ||
        code == "VideoUnFocus")           return "tampering";
    if (code == "AlarmLocal"  ||
        code == "FireWarning")            return "fire";
    if (code == "SmokeDetection")         return "smoke";
    if (code == "TakenAwayDetection")     return "object_taken";
    if (code == "LeftDetection")          return "object_left";
    return "hardware_alarm";
}

// BUG-EVENTS-01 fix (Dahua): real multipart long-poll subscription via
// `/cgi-bin/eventManager.cgi?action=attach`. The endpoint returns a
// chunked text stream with one event per chunk in `Code=X;action=Y;index=N;`
// shape, separated by mime boundaries. Heartbeat=5 keeps the connection
// alive so a quiet camera doesn't get dropped by Dahua's idle timeout.
//
// We accumulate partial-chunk text across callback invocations until we hit a
// newline; only then do we parse, so a Code= line split across two TCP reads
// still gets reconstructed.
//
// Connection lifecycle is owned by the caller (CameraEventService::workerLoop):
// when its session->stop_flag flips the callback returns false, streamWriteCallback
// signals abort to libcurl, and we unwind. The outer loop reconnects after
// a 5s grace.
void DahuaCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    CameraDiscovery::HttpClient http(cfg);
    std::string buffer; // Cross-chunk reassembly buffer

    LOG_INFO("[DahuaCore] Subscribing to events on {}:{}", cfg.host, cfg.http_port);

    // codes=[All] subscribes to every event class the device supports. We do
    // brand-side filtering instead of letting the camera pre-filter so that
    // any new firmware-introduced event type still flows through unchanged.
    http.streamGet("/cgi-bin/eventManager.cgi?action=attach&codes=[All]&heartbeat=5",
        [&](const std::string& chunk) -> bool {
            buffer.append(chunk);

            // Process every complete line, leave the trailing partial line
            // (if any) in the buffer for the next callback.
            size_t pos = 0;
            while (true) {
                size_t nl = buffer.find('\n', pos);
                if (nl == std::string::npos) break;
                std::string line = buffer.substr(pos, nl - pos);
                pos = nl + 1;

                // Strip trailing \r — Dahua uses CRLF line endings inside
                // mime parts.
                if (!line.empty() && line.back() == '\r') line.pop_back();

                // Only `Code=...;` lines carry events. Header lines (mime
                // boundary, Content-Type, Content-Length) and heartbeat
                // `Heartbeat` keepalives are skipped.
                if (line.rfind("Code=", 0) != 0) continue;

                // Parse `key=value;` pairs.
                std::string code, action, index, data;
                size_t i = 0;
                while (i < line.size()) {
                    size_t eq = line.find('=', i);
                    if (eq == std::string::npos) break;
                    size_t semi = line.find(';', eq);
                    if (semi == std::string::npos) semi = line.size();
                    std::string key = line.substr(i, eq - i);
                    std::string val = line.substr(eq + 1, semi - eq - 1);
                    if      (key == "Code")   code   = val;
                    else if (key == "action") action = val;
                    else if (key == "index")  index  = val;
                    else if (key == "data")   data   = val;
                    i = semi + 1;
                }

                if (code.empty()) continue;
                // `Pulse` and `Stop` are valid Dahua action verbs alongside
                // `Start`. We treat Start/Pulse as active=true (alarm raised),
                // Stop as active=false (alarm cleared). Anything else we let
                // through with active=true so the operator still sees it.
                bool active = (action != "Stop");

                int channel = 0;
                try { channel = std::stoi(index); } catch (...) {}

                nlohmann::json envelope = {
                    {"type",        "camera_event"},
                    {"brand",       "Dahua"},
                    {"event_type",  dahuaCodeToEventType(code)},
                    {"active",      active},
                    {"channel",     channel},
                    {"native_code", code},
                    {"native_action", action.empty() ? "Start" : action},
                    {"timestamp",   (long long)std::time(nullptr)}
                };
                if (!data.empty()) envelope["data"] = data;

                if (!onEvent(envelope.dump())) {
                    // Caller asked us to stop — drain the buffer and bail.
                    buffer.clear();
                    return false;
                }
            }

            // Trim consumed prefix so the buffer doesn't grow without bound
            // if the camera never emits a newline (pathological case).
            if (pos > 0) buffer.erase(0, pos);
            // Guard against unbounded growth on a broken stream.
            if (buffer.size() > 64 * 1024) {
                LOG_WARN("[DahuaCore] Event stream produced {} bytes without "
                         "a newline — dropping buffer to recover.", buffer.size());
                buffer.clear();
            }
            return true;
        },
        /*digest=*/true,
        ""
    );

    LOG_INFO("[DahuaCore] Event stream closed for {}:{}", cfg.host, cfg.http_port);
}

} // namespace brands
} // namespace core
} // namespace vms
