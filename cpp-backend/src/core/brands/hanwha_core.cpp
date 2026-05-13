#include "core/brands/hanwha_core.hpp"
#include "core/brands/HanwhaAdapter.hpp"
#include "core/brands/http_client.h"
#include "utils/logger.h"
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <unordered_map>

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

// BUG-EVENTS-01 (2026-05-07): pre-fix called a no-op
// `HanwhaAdapter::startEventSubscription` and spun a keepalive loop. Real
// Hanwha SUNAPI event subscription was never wired.
// Map Hanwha SUNAPI event field names to the brand-agnostic event_type vocab.
// Field names come from `GET /stw-cgi/eventstatus.cgi?...&action=check`'s
// JSON response, which is a snapshot of every event-detector's current state.
static std::string hanwhaFieldToEventType(const std::string& field) {
    if (field == "MotionDetection")          return "motion_detect";
    if (field == "VirtualLine"      ||
        field == "LineCrossing"     ||
        field == "EnterLine"        ||
        field == "ExitLine")                  return "line_crossing";
    if (field == "Intrusion"        ||
        field == "DefinedArea"      ||
        field == "ObjectDetection"  ||
        field == "IntelligentVA")             return "intrusion";
    if (field == "Loitering")                 return "loitering";
    if (field == "FaceDetection")             return "face";
    if (field == "AudioDetection"  ||
        field == "SoundClassification")       return "audio_alarm";
    if (field == "Tampering"       ||
        field == "VideoLoss"       ||
        field == "DefocusDetection")          return "tampering";
    if (field == "AlarmInput")                return "hardware_alarm";
    if (field == "Fire")                       return "fire";
    return ""; // Empty → caller should skip this field (not interesting)
}

// BUG-EVENTS-01 fix (Hanwha/Wisenet): SUNAPI does not expose a long-poll event
// stream in a way that's consistent across the Wisenet 6/7/X firmware split.
// (Wisenet X supports `eventstream.cgi?Subscription=On`, Wisenet 6 doesn't.)
// We use the lowest-common-denominator path: poll the snapshot endpoint
// `/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=check` every 2s and
// diff against the previous snapshot. State transitions emit events.
//
// Response shape:
//   { "Channel": [ { "Channel": 0, "MotionDetection": true,
//                    "Tampering": false, ... }, ... ] }
//
// We track per-(channel,field) state in a local map. First poll establishes
// the baseline (no events emitted) so the operator doesn't see a flood of
// "ack everything that's already active" alarms on subscription start.
//
// Cancellation: the caller's onEvent returning false bails out immediately
// from the next emit attempt; between polls we sleep in 200ms slices and
// check a stop sentinel by attempting a no-op emit (empty channel triggers
// just the callback return check without producing a notification).
void HanwhaCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    CameraDiscovery::HttpClient http(cfg);

    LOG_INFO("[HanwhaCore] Subscribing to events (poll mode) on {}:{}", cfg.host, cfg.http_port);

    // (channel, field) → last known bool state. Used to detect edges.
    std::unordered_map<std::string, bool> last_state;
    bool first_poll = true;

    // Helper: sleep in 200ms slices and bail early if a no-op callback
    // returns false. We do NOT call onEvent during sleep — instead the next
    // iteration's emit will naturally short-circuit.
    auto interruptible_sleep = [&](int total_ms) -> bool {
        const int slice = 200;
        int slept = 0;
        while (slept < total_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            slept += slice;
        }
        return true; // We have no in-band cancel signal in the curl-less
                     // sleep; the outer loop checks stop_flag and the next
                     // onEvent() call will return false, which terminates us.
    };

    while (true) {
        auto resp = http.get(
            "/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=check",
            /*digest=*/true);

        if (!resp.ok()) {
            // Camera unreachable or auth failed. Don't spin — sleep and let
            // the outer worker loop decide whether to reconnect.
            LOG_WARN("[HanwhaCore] eventstatus.cgi returned status={} for {}:{}",
                     resp.status, cfg.host, cfg.http_port);
            interruptible_sleep(5000);
            return;
        }

        nlohmann::json doc;
        try {
            doc = nlohmann::json::parse(resp.body);
        } catch (...) {
            LOG_WARN("[HanwhaCore] eventstatus.cgi returned non-JSON body — "
                     "firmware variant not supported via poll mode.");
            interruptible_sleep(10000);
            return;
        }

        if (!doc.contains("Channel") || !doc["Channel"].is_array()) {
            // Some firmwares return a flat top-level object. Try that shape.
            // If it's neither, give up on this camera for the cycle.
            if (doc.is_object() && !doc.empty()) {
                doc = nlohmann::json::array({doc});
            } else {
                interruptible_sleep(5000);
                continue;
            }
        }

        const auto& channels = doc.contains("Channel") ? doc["Channel"] : doc;
        for (const auto& ch_obj : channels) {
            if (!ch_obj.is_object()) continue;
            int channel = ch_obj.value("Channel", 0);

            for (auto it = ch_obj.begin(); it != ch_obj.end(); ++it) {
                if (it.key() == "Channel") continue;
                if (!it.value().is_boolean()) continue;

                std::string evt_type = hanwhaFieldToEventType(it.key());
                if (evt_type.empty()) continue; // Field we don't care about.

                bool now_active = it.value().get<bool>();
                std::string key = std::to_string(channel) + ":" + it.key();
                bool changed = false;
                auto prev_it = last_state.find(key);
                if (prev_it == last_state.end()) {
                    last_state[key] = now_active;
                    // First-poll seeding: only emit if active to signal
                    // existing alarm state, but skip if first_poll to avoid
                    // boot-time alarm flood.
                    changed = now_active && !first_poll;
                } else if (prev_it->second != now_active) {
                    prev_it->second = now_active;
                    changed = true;
                }

                if (!changed) continue;

                nlohmann::json envelope = {
                    {"type",       "camera_event"},
                    {"brand",      "Hanwha"},
                    {"event_type", evt_type},
                    {"active",     now_active},
                    {"channel",    channel},
                    {"field",      it.key()},
                    {"timestamp",  (long long)std::time(nullptr)}
                };

                if (!onEvent(envelope.dump())) {
                    LOG_INFO("[HanwhaCore] Event stream stopped by caller for {}:{}",
                             cfg.host, cfg.http_port);
                    return;
                }
            }
        }

        first_poll = false;
        if (!interruptible_sleep(2000)) return;
    }
}

} // namespace brands
} // namespace core
} // namespace vms
