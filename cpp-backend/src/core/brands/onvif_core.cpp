#include "core/brands/onvif_core.hpp"
#include "core/brands/http_client.h"
#include "utils/logger.h"

using json = nlohmann::json;
#include <pugixml.hpp>
#include <chrono>
#include <thread>

namespace vms {
namespace core {
namespace brands {

bool ONVIFCore::probe(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::HttpClient http(cfg);
    auto resp = http.post("/onvif/device_service",
        buildSoap("GetDeviceInformation",
                  "http://www.onvif.org/ver10/device/wsdl",
                  cfg.username, cfg.password),
        false, "application/soap+xml");
    return resp.ok() && resp.body.find("Manufacturer") != std::string::npos;
}

CameraDiscovery::DeviceInfo ONVIFCore::discover(const CameraDiscovery::DiscoveryConfig& cfg) {
    CameraDiscovery::DeviceInfo dev;
    dev.brand = CameraDiscovery::Brand::ONVIF;
    CameraDiscovery::HttpClient http(cfg);

    // --- Device info ---
    auto resp = http.post("/onvif/device_service",
        buildSoap("GetDeviceInformation",
                  "http://www.onvif.org/ver10/device/wsdl",
                  cfg.username, cfg.password),
        false, "application/soap+xml");
    if (!resp.ok()) { dev.error = "Cannot reach ONVIF device service"; return dev; }

    pugi::xml_document doc;
    doc.load_string(resp.body.c_str());
    auto body_node = doc.select_node("//GetDeviceInformationResponse").node();
    dev.model    = body_node.child_value("Model");
    dev.serial   = body_node.child_value("SerialNumber");
    dev.firmware = body_node.child_value("FirmwareVersion");

    dev.cameras = getOptimizedStreams(cfg);
    dev.channels = (int)dev.cameras.size();
    return dev;
}

std::vector<CameraDiscovery::CameraChannel> ONVIFCore::getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) {
    std::vector<CameraDiscovery::CameraChannel> cameras;
    CameraDiscovery::HttpClient http(cfg);
    
    // --- Profiles ---
    auto prof_resp = http.post("/onvif/media_service",
        buildSoap("GetProfiles",
                  "http://www.onvif.org/ver10/media/wsdl",
                  cfg.username, cfg.password),
        false, "application/soap+xml");
    if (!prof_resp.ok()) return cameras;

    pugi::xml_document prof_doc;
    prof_doc.load_string(prof_resp.body.c_str());

    int ch_idx = 1;
    for (auto& profile : prof_doc.select_nodes("//Profiles")) {
        std::string token = profile.node().attribute("token").as_string();
        if (token.empty()) continue;

        // GetStreamUri for each profile
        std::string body_xml =
            R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
  xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">
  <s:Header>)" + wsseHeader(cfg.username, cfg.password) + R"(</s:Header>
  <s:Body>
    <GetStreamUri xmlns="http://www.onvif.org/ver10/media/wsdl">
      <StreamSetup>
        <Stream xmlns="http://www.onvif.org/ver10/schema">RTP-Unicast</Stream>
        <Transport xmlns="http://www.onvif.org/ver10/schema">
          <Protocol>RTSP</Protocol>
        </Transport>
      </StreamSetup>
      <ProfileToken>)" + token + R"(</ProfileToken>
    </GetStreamUri>
  </s:Body>
</s:Envelope>)";

        auto uri_resp = http.post("/onvif/media_service", body_xml,
                                  false, "application/soap+xml");
        if (!uri_resp.ok()) continue;

        pugi::xml_document uri_doc;
        uri_doc.load_string(uri_resp.body.c_str());
        std::string rtsp_url = uri_doc.select_node("//Uri").node().child_value();
        if (rtsp_url.empty()) continue;

        // inject credentials into RTSP URL if missing
        if (rtsp_url.find('@') == std::string::npos) {
            auto pos = rtsp_url.find("://");
            if (pos != std::string::npos)
                rtsp_url.insert(pos + 3, cfg.username + ":" + cfg.password + "@");
        }

        CameraDiscovery::CameraChannel cam;
        cam.channel_id = ch_idx;
        cam.name       = profile.node().child_value("Name");
        if (cam.name.empty()) cam.name = "Channel " + std::to_string(ch_idx);
        cam.online = true;

        if (ch_idx == 1 || cameras.empty()) {
            cam.rtsp_main = rtsp_url;
            cameras.push_back(cam);
        } else {
            if (!cameras.empty() && cameras.back().rtsp_sub.empty())
                cameras.back().rtsp_sub = rtsp_url;
            else {
                cam.rtsp_main = rtsp_url;
                cameras.push_back(cam);
            }
        }
        ch_idx++;
    }
    
    return cameras;
}

json ONVIFCore::getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
    return json::object();
}

std::string ONVIFCore::wsseHeader(const std::string& user, const std::string& pass) {
    return R"(<wsse:Security s:mustUnderstand="1">
  <wsse:UsernameToken>
    <wsse:Username>)" + user + R"(</wsse:Username>
    <wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordText">)"
               + pass + R"(</wsse:Password>
  </wsse:UsernameToken>
</wsse:Security>)";
}

std::string ONVIFCore::buildSoap(const std::string& method,
                             const std::string& xmlns,
                             const std::string& user,
                             const std::string& pass)
{
    return R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
  xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">
  <s:Header>)" + wsseHeader(user, pass) + R"(</s:Header>
  <s:Body>
    <)" + method + R"( xmlns=")" + xmlns + R"("/>
  </s:Body>
</s:Envelope>)";
}

bool ONVIFCore::ptzControl(const CameraDiscovery::DiscoveryConfig& cfg, const std::string& action, double speed) {
    CameraDiscovery::HttpClient http(cfg);
    
    // First get a profile token
    auto prof_resp = http.post("/onvif/media_service",
        buildSoap("GetProfiles", "http://www.onvif.org/ver10/media/wsdl", cfg.username, cfg.password),
        false, "application/soap+xml");
    if (!prof_resp.ok()) return false;

    pugi::xml_document doc;
    doc.load_string(prof_resp.body.c_str());
    std::string token;
    if (auto node = doc.select_node("//Profiles").node()) {
        token = node.attribute("token").as_string();
    }
    if (token.empty()) return false;

    float pan = 0, tilt = 0, zoom = 0;
    if (action == "up") tilt = speed;
    else if (action == "down") tilt = -speed;
    else if (action == "left") pan = -speed;
    else if (action == "right") pan = speed;
    else if (action == "zoom_in") zoom = speed;
    else if (action == "zoom_out") zoom = -speed;

    std::string body;
    if (action == "stop") {
        body = R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl">
  <s:Header>)" + wsseHeader(cfg.username, cfg.password) + R"(</s:Header>
  <s:Body>
    <tptz:Stop>
      <tptz:ProfileToken>)" + token + R"(</tptz:ProfileToken>
      <tptz:PanTilt>true</tptz:PanTilt>
      <tptz:Zoom>true</tptz:Zoom>
    </tptz:Stop>
  </s:Body>
</s:Envelope>)";
    } else {
        body = R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Header>)" + wsseHeader(cfg.username, cfg.password) + R"(</s:Header>
  <s:Body>
    <tptz:ContinuousMove>
      <tptz:ProfileToken>)" + token + R"(</tptz:ProfileToken>
      <tptz:Velocity>
        <tt:PanTilt x=")" + std::to_string(pan) + R"(" y=")" + std::to_string(tilt) + R"(" space="http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"/>
        <tt:Zoom x=")" + std::to_string(zoom) + R"(" space="http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace"/>
      </tptz:Velocity>
    </tptz:ContinuousMove>
  </s:Body>
</s:Envelope>)";
    }

    auto resp = http.post("/onvif/ptz_service", body, false, "application/soap+xml");
    return resp.ok();
}

// Map ONVIF Topic strings → brand-agnostic event_type vocabulary.
// Topics use `tns1:` (ONVIF schema) and sometimes vendor extensions like
// `tnsaxis:` / `tns-h:`. We match on substring of the trailing topic segments
// to stay vendor-agnostic.
static std::string onvifTopicToEventType(const std::string& topic) {
    auto contains = [&](const char* needle) { return topic.find(needle) != std::string::npos; };
    if (contains("CellMotion") ||
        contains("MotionAlarm") ||
        contains("MotionDetect"))          return "motion_detect";
    if (contains("LineDetector") ||
        contains("LineCrossing"))           return "line_crossing";
    if (contains("FieldDetector") ||
        contains("ObjectsInside") ||
        contains("Intrusion"))              return "intrusion";
    if (contains("LoiteringDetector") ||
        contains("Loitering"))              return "loitering";
    if (contains("Tampering") ||
        contains("SceneChange") ||
        contains("ImageTooBlurry") ||
        contains("ImageTooDark"))           return "tampering";
    if (contains("AudioSource") ||
        contains("AudioDetection") ||
        contains("AudioState"))             return "audio_alarm";
    if (contains("FaceDetect"))             return "face";
    if (contains("DigitalInput") ||
        contains("Trigger"))                return "hardware_alarm";
    if (contains("Fire"))                   return "fire";
    if (contains("Smoke"))                  return "smoke";
    return "hardware_alarm";
}

// Extract the path component from a SubscriptionReference Address URL. ONVIF
// returns an absolute URL like `http://192.168.1.10/onvif/Subscription?Idx=7`;
// we feed only the path-and-query into HttpClient so the host/port stays
// pinned to cfg_ (no leak across cameras).
static std::string onvifSubscriptionPath(const std::string& addr) {
    auto pos = addr.find("://");
    if (pos == std::string::npos) return addr; // already a path
    pos = addr.find('/', pos + 3);
    if (pos == std::string::npos) return "/";
    return addr.substr(pos);
}

// BUG-EVENTS-01 fix (ONVIF): the previous body sent a single
// `GetSystemDateAndTime` SOAP probe and then spun a 1-second
// `onEvent("keepalive")` loop forever. It NEVER subscribed to real ONVIF
// events — operators configuring an ONVIF camera saw "polling events for cam X"
// in logs but no motion / line-crossing / intrusion alerts ever arrived.
//
// Now: full WS-BaseNotification + PullPoint flow:
//   1. POST `CreatePullPointSubscription` to `/onvif/event_service`
//      → device returns a `SubscriptionReference` Address URL pointing to a
//        per-subscriber endpoint (e.g. `/onvif/Subscription?Idx=N`).
//   2. Loop: POST `PullMessages` to that endpoint with a 5s Timeout. The camera
//      returns up to MessageLimit `NotificationMessage`s, blocking up to the
//      Timeout when no events are pending. We bound at 5s so cancellation
//      (onEvent returning false) is felt within one round-trip.
//   3. Parse each NotificationMessage's Topic + Source + Data SimpleItems,
//      normalize via `onvifTopicToEventType`, emit the common envelope.
//   4. On exit (caller cancellation OR camera disconnect): POST `Unsubscribe`
//      to release the device-side subscription slot. ONVIF spec lets cameras
//      reclaim slots via TerminationTime so this isn't critical, but it's
//      polite and avoids slow leaks on chatty subscribe/unsubscribe cycles.
void ONVIFCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    CameraDiscovery::HttpClient http(cfg);

    // ── Step 1: CreatePullPointSubscription ──────────────────────────────
    std::string create_body =
        R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
             xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
             xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2"
             xmlns:tev="http://www.onvif.org/ver10/events/wsdl">
          <s:Header>)" + wsseHeader(cfg.username, cfg.password) + R"(</s:Header>
          <s:Body>
            <tev:CreatePullPointSubscription>
              <tev:InitialTerminationTime>PT1H</tev:InitialTerminationTime>
            </tev:CreatePullPointSubscription>
          </s:Body>
        </s:Envelope>)";

    auto create_resp = http.post("/onvif/event_service", create_body,
                                 /*digest=*/false, "application/soap+xml");
    if (!create_resp.ok()) {
        LOG_WARN("[ONVIFCore] CreatePullPointSubscription failed on {}:{} status={} "
                 "— event subscription unavailable, sleeping 30s before retry.",
                 cfg.host, cfg.http_port, create_resp.status);
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return;
    }

    pugi::xml_document create_doc;
    if (!create_doc.load_string(create_resp.body.c_str())) {
        LOG_WARN("[ONVIFCore] CreatePullPointSubscription returned unparseable XML.");
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return;
    }

    std::string subscription_addr;
    if (auto addr_node = create_doc.select_node("//SubscriptionReference/Address").node()) {
        subscription_addr = addr_node.child_value();
    }
    if (subscription_addr.empty()) {
        LOG_WARN("[ONVIFCore] No SubscriptionReference/Address in response — device does not advertise PullPoint.");
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return;
    }
    std::string sub_path = onvifSubscriptionPath(subscription_addr);
    LOG_INFO("[ONVIFCore] Pull-point subscription established at {}", sub_path);

    // ── Step 2: PullMessages loop ────────────────────────────────────────
    bool caller_cancelled = false;
    while (!caller_cancelled) {
        std::string pull_body =
            R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
                 xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
                 xmlns:tev="http://www.onvif.org/ver10/events/wsdl">
              <s:Header>)" + wsseHeader(cfg.username, cfg.password) + R"(</s:Header>
              <s:Body>
                <tev:PullMessages>
                  <tev:Timeout>PT5S</tev:Timeout>
                  <tev:MessageLimit>100</tev:MessageLimit>
                </tev:PullMessages>
              </s:Body>
            </s:Envelope>)";

        auto pull_resp = http.post(sub_path, pull_body,
                                   /*digest=*/false, "application/soap+xml");

        if (!pull_resp.ok()) {
            // Camera disconnected / subscription expired. Bail out so the
            // outer worker loop reconnects from CreatePullPointSubscription.
            LOG_WARN("[ONVIFCore] PullMessages returned status={} on {}:{} — "
                     "subscription dropped, will re-establish.",
                     pull_resp.status, cfg.host, cfg.http_port);
            break;
        }

        pugi::xml_document pull_doc;
        if (!pull_doc.load_string(pull_resp.body.c_str())) {
            // Couldn't parse — skip this round but keep the subscription open.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        for (auto& nm : pull_doc.select_nodes(".//NotificationMessage")) {
            auto nm_node = nm.node();
            std::string topic = nm_node.child_value("Topic");
            if (topic.empty()) {
                // Defensive: some devices nest Topic under wsnt:.
                if (auto t = nm_node.select_node(".//Topic").node()) topic = t.child_value();
            }
            if (topic.empty()) continue;

            // ONVIF data convention: `<Data><SimpleItem Name="State|IsMotion|..." Value="true|false|1|0"/></Data>`.
            bool active = true;
            for (auto& si : nm_node.select_nodes(".//Message//Data//SimpleItem")) {
                std::string v = si.node().attribute("Value").as_string();
                if (v == "0" || v == "false" || v == "False") active = false;
            }

            std::string source_val;
            for (auto& si : nm_node.select_nodes(".//Message//Source//SimpleItem")) {
                source_val = si.node().attribute("Value").as_string();
                if (!source_val.empty()) break;
            }

            // Extract channel from source token (often "VideoSourceConfigToken_1"
            // or just "1"). Fall back to 0.
            int channel = 0;
            for (auto it = source_val.rbegin(); it != source_val.rend(); ++it) {
                if (!std::isdigit(static_cast<unsigned char>(*it))) {
                    size_t pos = source_val.size() - (it - source_val.rbegin());
                    if (pos < source_val.size()) {
                        try { channel = std::stoi(source_val.substr(pos)); } catch (...) {}
                    }
                    break;
                }
            }

            nlohmann::json envelope = {
                {"type",       "camera_event"},
                {"brand",      "ONVIF"},
                {"event_type", onvifTopicToEventType(topic)},
                {"active",     active},
                {"channel",    channel},
                {"topic",      topic},
                {"source",     source_val},
                {"timestamp",  (long long)std::time(nullptr)}
            };

            if (!onEvent(envelope.dump())) {
                caller_cancelled = true;
                break;
            }
        }
    }

    // ── Step 3: Unsubscribe (best-effort) ────────────────────────────────
    std::string unsub_body =
        R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
             xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
             xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2">
          <s:Header>)" + wsseHeader(cfg.username, cfg.password) + R"(</s:Header>
          <s:Body><wsnt:Unsubscribe/></s:Body>
        </s:Envelope>)";
    http.post(sub_path, unsub_body, /*digest=*/false, "application/soap+xml");
    LOG_INFO("[ONVIFCore] Event subscription closed for {}:{}", cfg.host, cfg.http_port);
}

} // namespace brands
} // namespace core
} // namespace vms
