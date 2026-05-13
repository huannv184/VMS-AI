#include "core/brands/axis_core.hpp"
#include "core/brands/AxisAdapter.hpp"
#include "core/brands/http_client.h"
#include "utils/logger.h"
#include <pugixml.hpp>
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
// Map Axis VAPIX topic strings to the brand-agnostic event_type vocabulary.
// Topic shape:  `tns1:<Producer>/tnsaxis:<Name>`  or  `tns1:<Producer>/<Name>`.
// We substring-match on the trailing Name only, since the producer prefix
// drifts between firmware versions (tns1: vs tnsaxis: vs PROXY:...).
static std::string axisTopicToEventType(const std::string& topic) {
    auto contains = [&](const char* needle) { return topic.find(needle) != std::string::npos; };
    if (contains("MotionDetection") ||
        contains("MotionRegion")     ||
        contains("VMD"))              return "motion_detect";
    if (contains("LineDetection")   ||
        contains("LineCross")        ||
        contains("Fence"))            return "line_crossing";
    if (contains("Intrusion")       ||
        contains("MotionGuard"))      return "intrusion";
    if (contains("LoiteringGuard"))   return "loitering";
    if (contains("Tampering")       ||
        contains("DayNightVision"))   return "tampering";
    if (contains("AudioDetection")  ||
        contains("AudioSource"))      return "audio_alarm";
    if (contains("FaceDetect"))       return "face";
    if (contains("AlarmInput")      ||
        contains("Digital/Input"))    return "hardware_alarm";
    if (contains("Fire"))             return "fire";
    return "hardware_alarm";
}

// BUG-EVENTS-01 fix (Axis): real VAPIX 3 event subscription via
// `/axis-cgi/eventfeed.cgi?Stream=Most`. Axis returns multipart/x-mixed-replace
// with each event as a `<MetaData>` XML block. We accumulate raw text across
// chunked TCP reads, parse with pugixml whenever we see a `</MetaData>` close
// tag, then route through `axisTopicToEventType` to the common envelope.
//
// `keep_alive_period=10` makes the camera emit a heartbeat block every 10s so
// HttpClient::streamGet's LOW_SPEED_TIME=15s guard doesn't tear the connection
// on a quiet site. Heartbeat blocks have no NotificationMessage child and are
// dropped after parse.
//
// Axis VAPIX 3 has a richer SOAP/WS-Pull alternative under `/vapix/services`,
// but the eventfeed.cgi flat stream is documented support'd across all VAPIX 3
// firmware (G3-class onward, ~2014+) and avoids the SOAP subscription dance.
void AxisCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    CameraDiscovery::HttpClient http(cfg);
    std::string buffer;

    LOG_INFO("[AxisCore] Subscribing to events on {}:{}", cfg.host, cfg.http_port);

    http.streamGet("/axis-cgi/eventfeed.cgi?Stream=Most&keep_alive_period=10",
        [&](const std::string& chunk) -> bool {
            buffer.append(chunk);

            // Reassemble complete <MetaData>...</MetaData> blocks. We process
            // every closed block in the buffer, leaving any trailing partial
            // block for the next callback.
            while (true) {
                size_t open  = buffer.find("<MetaData");
                if (open == std::string::npos) {
                    // No open tag → safe to drop everything (mime boundary +
                    // headers we don't care about). But cap to avoid surprise.
                    if (buffer.size() > 64 * 1024) buffer.clear();
                    break;
                }
                size_t close = buffer.find("</MetaData>", open);
                if (close == std::string::npos) {
                    // Block straddles a chunk — discard pre-open junk so
                    // the buffer doesn't grow unbounded but keep the open tag.
                    if (open > 0) buffer.erase(0, open);
                    if (buffer.size() > 256 * 1024) {
                        LOG_WARN("[AxisCore] MetaData block grew past 256KB "
                                 "without closing — dropping buffer.");
                        buffer.clear();
                    }
                    break;
                }
                size_t end = close + std::string("</MetaData>").size();
                std::string block = buffer.substr(open, end - open);
                buffer.erase(0, end);

                pugi::xml_document doc;
                if (!doc.load_string(block.c_str())) continue;

                // Walk every NotificationMessage — a single MetaData block can
                // carry several stacked events on busy cameras.
                for (auto& nm : doc.select_nodes(".//NotificationMessage")) {
                    auto nm_node = nm.node();
                    std::string topic = nm_node.child_value("Topic");
                    if (topic.empty()) continue;

                    // Extract the data state value. The convention is
                    // `<Data><SimpleItem Name="state|active|motion" Value="0|1|true|false"/></Data>`.
                    bool active = true;
                    std::string source_val;
                    for (auto& si : nm_node.select_nodes(".//Message//Data//SimpleItem")) {
                        std::string v = si.node().attribute("Value").as_string();
                        if (v == "0" || v == "false" || v == "False") active = false;
                        // 1 / true → keep active=true (default)
                    }
                    for (auto& si : nm_node.select_nodes(".//Message//Source//SimpleItem")) {
                        source_val = si.node().attribute("Value").as_string();
                        if (!source_val.empty()) break;
                    }

                    int channel = 0;
                    try { channel = std::stoi(source_val); } catch (...) {}

                    nlohmann::json envelope = {
                        {"type",       "camera_event"},
                        {"brand",      "Axis"},
                        {"event_type", axisTopicToEventType(topic)},
                        {"active",     active},
                        {"channel",    channel},
                        {"topic",      topic},
                        {"timestamp",  (long long)std::time(nullptr)}
                    };

                    if (!onEvent(envelope.dump())) {
                        buffer.clear();
                        return false;
                    }
                }
            }
            return true;
        },
        /*digest=*/true,
        ""
    );

    LOG_INFO("[AxisCore] Event stream closed for {}:{}", cfg.host, cfg.http_port);
}

} // namespace brands
} // namespace core
} // namespace vms
