#include "core/brands/onvif_core.hpp"
#include "core/brands/http_client.h"

using json = nlohmann::json;
#include "core/brands/http_client.h"
#include <pugixml.hpp>  // TODO: Enable after vcpkg setup

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

void ONVIFCore::pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
    CameraDiscovery::HttpClient http(cfg);
    auto resp = http.post("/onvif/device_service",
        buildSoap("GetSystemDateAndTime", "http://www.onvif.org/ver10/device/wsdl", cfg.username, cfg.password),
        false, "application/soap+xml");

    if (!resp.ok()) return;

    bool isRunning = true;
    while (isRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!onEvent("keepalive")) {
            isRunning = false;
        }
    }
}

} // namespace brands
} // namespace core
} // namespace vms
