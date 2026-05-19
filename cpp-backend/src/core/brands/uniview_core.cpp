#include "core/brands/uniview_core.hpp"
#include "core/brands/http_client.h"
#include "core/runtime_state.h"
#include "utils/logger.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace vms {
namespace core {
namespace brands {

bool UniviewCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    auto resp = http.get("/LAPI/V1.0/System/DeviceBasicInfo");
    return resp.ok() && resp.body.find("SerialNumber") != std::string::npos;
}

CameraDiscovery::DeviceInfo UniviewCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::Uniview;
    CameraDiscovery::HttpClient http(cfg);

    auto resp = http.get("/LAPI/V1.0/System/DeviceBasicInfo");
    if (!resp.ok()) { dev.error = "Cannot reach Uniview LAPI"; return dev; }

    try {
        auto j       = json::parse(resp.body);
        auto info    = j.value("Response", json::object()).value("Data", json::object());
        dev.model    = info.value("Model",        "");
        dev.serial   = info.value("SerialNumber", "");
        dev.firmware = info.value("SoftwareVersion", "");
    } catch (...) { dev.error = "Uniview LAPI JSON parse error"; return dev; }

    dev.cameras = getOptimizedStreams(cfg);
    dev.channels = (int)dev.cameras.size();
    return dev;
}

std::vector<CameraDiscovery::CameraChannel> UniviewCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    CameraDiscovery::HttpClient http(cfg);
    
    auto ch_resp = http.get("/LAPI/V1.0/Channels/VideoIn");
    std::string base = "rtsp://" + cfg.username + ":" + cfg.password +
                       "@" + cfg.host + ":" + std::to_string(cfg.rtsp_port);

    if (ch_resp.ok()) {
        try {
            auto j = json::parse(ch_resp.body);
            auto data = j.value("Response", json::object()).value("Data", json::object());
            if (data.contains("VideoInItem")) {
                for (auto& item : data["VideoInItem"]) {
                    int ch = item.value("ID", 1);
                    CameraDiscovery::CameraChannel cam;
                    cam.channel_id = ch;
                    cam.name       = "Uniview Camera " + std::to_string(ch);
                    cam.online     = true;
                    cam.rtsp_main  = base + "/media/video" + std::to_string(ch);
                    cam.rtsp_sub   = base + "/media/video" + std::to_string(ch) + "/sub";
                    cameras.push_back(cam);
                }
            }
        } catch (...) { LOG_DEBUG("UniviewCore: Failed to parse channel list JSON"); }
    }

    if (cameras.empty()) {
        CameraDiscovery::CameraChannel cam;
        cam.channel_id = 1; cam.name = "Uniview Main Camera"; cam.online = true;
        cam.rtsp_main  = base + "/media/video1";
        cam.rtsp_sub   = base + "/media/video1/sub";
        cameras.push_back(cam);
    }
    
    return cameras;
}

nlohmann::json UniviewCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    return nlohmann::json::object();
}

void UniviewCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    CameraDiscovery::HttpClient http(cfg);
    // Uniview LAPI HTTP-stream subscription. Each chunk that contains an
    // `EventType` key is forwarded to onEvent. onEvent's return value is
    // honoured — returning false breaks the stream.
    const std::string path = "/LAPI/V1.0/System/Event/Subscription";

    // 2026-05-19 BUG-UNIVIEW-LOOP-01 fix: pre-fix used `bool running = true`
    // that was NEVER mutated, so a streamGet exit (camera reboot, auth
    // expired, network drop) sleep-5s'd and retried forever — thread leak.
    // The fix mirrors the brand-events sprint pattern (Axis / ONVIF /
    // Dahua / Hanwha): bounded retry loop with shutdown gate + exponential
    // backoff, cap at 5 consecutive failures, log and exit so the caller
    // (CameraEventService) can decide whether to relaunch with backoff of
    // its own.
    int consecutive_failures = 0;
    constexpr int kMaxConsecutiveFailures = 5;
    auto backoff_ms = std::chrono::milliseconds(2000);
    constexpr auto kMaxBackoff = std::chrono::milliseconds(30'000);

    while (!vms::core::shutting_down.load(std::memory_order_acquire) &&
           consecutive_failures < kMaxConsecutiveFailures) {
        // streamGet returns void; any return path (clean disconnect, curl
        // error, onEvent returning false) means the long-poll connection
        // is no longer live. Caller-side restart with backoff is the only
        // recovery channel we have.
        http.streamGet(path,
            [&onEvent](const std::string& chunk) -> bool {
                if (chunk.find("\"EventType\"") != std::string::npos) {
                    return onEvent(chunk);
                }
                return true; // keep streaming through unrelated chunks
            });

        if (vms::core::shutting_down.load(std::memory_order_acquire)) break;

        ++consecutive_failures;
        LOG_WARN("[UniviewCore] event stream ended on {} (#{}/{}), "
                 "backing off {} ms",
                 cfg.host, consecutive_failures, kMaxConsecutiveFailures,
                 static_cast<long long>(backoff_ms.count()));
        std::this_thread::sleep_for(backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, kMaxBackoff);
    }

    if (consecutive_failures >= kMaxConsecutiveFailures) {
        LOG_WARN("[UniviewCore] giving up event subscription on {} after "
                 "{} consecutive failures — CameraEventService may relaunch "
                 "with its own outer backoff",
                 cfg.host, kMaxConsecutiveFailures);
    }
}

} // namespace brands
} // namespace core
} // namespace vms
