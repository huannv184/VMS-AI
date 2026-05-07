#include "core/ptz_manager.h"
#include "database/camera_repository.h"
#include "utils/logger.h"
#include "utils/http_digest_client.h"

#include <sstream>
#include <fstream>
#include <regex>
#include <algorithm>
#include <ctime>

namespace vms {
namespace core {

PTZManager& PTZManager::getInstance() {
    static PTZManager instance;
    return instance;
}

// ============================================================================
// Camera Info Resolution
// ============================================================================

PTZManager::CameraConnectionInfo PTZManager::getCameraInfo(int camera_id) {
    CameraConnectionInfo info;
    info.camera_id = camera_id;
    try {
        database::CameraRepository repo;
        auto cam_opt = repo.getCameraById(camera_id);
        if (!cam_opt) {
            LOG_WARN("[PTZ] Camera {} not found in database", camera_id);
            return info;
        }
        auto& cam = cam_opt.value();
        
        // Parse RTSP URL to extract IP and credentials
        // rtsp://user:pass@ip:port/path
        std::string url = cam.rtsp_url;
        std::regex rtsp_regex(R"(rtsp://(?:([^:]+):([^@]+)@)?([^:/]+)(?::(\d+))?)");
        std::smatch match;
        if (std::regex_search(url, match, rtsp_regex)) {
            if (match[1].matched) info.username = match[1].str();
            if (match[2].matched) info.password = match[2].str();
            info.ip = match[3].str();
            if (match[4].matched) info.port = std::stoi(match[4].str());
        }

        // Try to get brand from advanced_config or detect from URL
        if (!cam.advanced_config.empty()) {
            try {
                auto cfg = nlohmann::json::parse(cam.advanced_config);
                if (cfg.contains("brand")) info.brand = cfg["brand"].get<std::string>();
            } catch (...) {}
        }

        // Auto-detect brand from URL patterns
        if (info.brand.empty()) {
            std::string lower_url = url;
            std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), ::tolower);
            if (lower_url.find("isapi") != std::string::npos || 
                lower_url.find("streaming/channels") != std::string::npos) {
                info.brand = "hikvision";
            } else if (lower_url.find("cam/realmonitor") != std::string::npos) {
                info.brand = "dahua";
            }
        }

        info.protocol = detectProtocol(camera_id);

    } catch (const std::exception& e) {
        LOG_ERROR("[PTZ] Error getting camera info for {}: {}", camera_id, e.what());
    }
    return info;
}

// XML-escape user-supplied strings before splicing into SOAP/ISAPI envelopes.
// Operator-controlled names (preset name, etc.) flow through this — without
// the escape a name like `Lobby & Door 'A'` produces malformed XML and a
// hostile name could forge sibling elements (SOAP injection).
static std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '&':  out += "&amp;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

PTZProtocol PTZManager::detectProtocol(int camera_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = protocol_cache_.find(camera_id);
    if (it != protocol_cache_.end()) return it->second;

    auto info = getCameraInfo(camera_id);
    PTZProtocol proto = PTZProtocol::ONVIF; // Default

    std::string brand = info.brand;
    std::transform(brand.begin(), brand.end(), brand.begin(), ::tolower);

    if (brand.find("hikvision") != std::string::npos || brand.find("hik") != std::string::npos) {
        proto = PTZProtocol::HIKVISION_ISAPI;
    } else if (brand.find("dahua") != std::string::npos) {
        proto = PTZProtocol::DAHUA_CGI;
    }

    protocol_cache_[camera_id] = proto;
    return proto;
}

// ============================================================================
// Public API — dispatch to correct protocol
// ============================================================================

bool PTZManager::continuousMove(int camera_id, float pan_speed, float tilt_speed, float zoom_speed) {
    auto info = getCameraInfo(camera_id);
    LOG_INFO("[PTZ] ContinuousMove cam={} pan={:.2f} tilt={:.2f} zoom={:.2f}", 
             camera_id, pan_speed, tilt_speed, zoom_speed);

    switch (info.protocol) {
        case PTZProtocol::HIKVISION_ISAPI: return hikvisionMove(info, pan_speed, tilt_speed, zoom_speed);
        case PTZProtocol::DAHUA_CGI: return dahuaMove(info, pan_speed, tilt_speed, zoom_speed);
        case PTZProtocol::ONVIF: return onvifMove(info, pan_speed, tilt_speed, zoom_speed);
        default: 
            LOG_WARN("[PTZ] Unknown protocol for camera {}", camera_id);
            return false;
    }
}

bool PTZManager::stopMove(int camera_id) {
    auto info = getCameraInfo(camera_id);
    LOG_INFO("[PTZ] StopMove cam={}", camera_id);

    switch (info.protocol) {
        case PTZProtocol::HIKVISION_ISAPI: return hikvisionStop(info);
        case PTZProtocol::DAHUA_CGI: return dahuaStop(info);
        case PTZProtocol::ONVIF: return onvifStop(info);
        default: return false;
    }
}

bool PTZManager::absoluteMove(int camera_id, float pan, float tilt, float zoom) {
    LOG_INFO("[PTZ] AbsoluteMove cam={} pan={:.2f} tilt={:.2f} zoom={:.2f}", camera_id, pan, tilt, zoom);
    // For now, treat as continuous move with duration (simplified)
    return continuousMove(camera_id, pan, tilt, zoom);
}

bool PTZManager::relativeMove(int camera_id, float pan_delta, float tilt_delta, float zoom_delta) {
    LOG_INFO("[PTZ] RelativeMove cam={} dpan={:.2f} dtilt={:.2f} dzoom={:.2f}", 
             camera_id, pan_delta, tilt_delta, zoom_delta);
    return continuousMove(camera_id, pan_delta, tilt_delta, zoom_delta);
}

std::vector<PTZPreset> PTZManager::getPresets(int camera_id) {
    auto info = getCameraInfo(camera_id);
    switch (info.protocol) {
        case PTZProtocol::HIKVISION_ISAPI: return hikvisionGetPresets(info);
        case PTZProtocol::DAHUA_CGI: return dahuaGetPresets(info);
        case PTZProtocol::ONVIF: return onvifGetPresets(info);
        default: return {};
    }
}

bool PTZManager::gotoPreset(int camera_id, int preset_id) {
    auto info = getCameraInfo(camera_id);
    LOG_INFO("[PTZ] GotoPreset cam={} preset={}", camera_id, preset_id);
    switch (info.protocol) {
        case PTZProtocol::HIKVISION_ISAPI: return hikvisionGotoPreset(info, preset_id);
        case PTZProtocol::DAHUA_CGI: return dahuaGotoPreset(info, preset_id);
        case PTZProtocol::ONVIF: return onvifGotoPreset(info, preset_id);
        default: return false;
    }
}

bool PTZManager::startPatrol(int camera_id, int patrol_id) {
    auto info = getCameraInfo(camera_id);
    LOG_INFO("[PTZ] StartPatrol cam={} patrol={}", camera_id, patrol_id);
    switch (info.protocol) {
        case PTZProtocol::HIKVISION_ISAPI: return hikvisionStartPatrol(info, patrol_id);
        case PTZProtocol::DAHUA_CGI: return dahuaStartPatrol(info, patrol_id);
        case PTZProtocol::ONVIF: return onvifStartPatrol(info, patrol_id);
        default: return false;
    }
}

bool PTZManager::stopPatrol(int camera_id) {
    auto info = getCameraInfo(camera_id);
    LOG_INFO("[PTZ] StopPatrol cam={}", camera_id);
    // Most cameras stop patrol when a manual move command is sent or a specific stop
    switch (info.protocol) {
        case PTZProtocol::HIKVISION_ISAPI: {
            std::string url = "http://" + info.ip + ":" + std::to_string(info.port) + "/ISAPI/PTZCtrl/channels/1/patrols/1/stop";
            return !httpPut(url, "", info.username, info.password).empty();
        }
        case PTZProtocol::DAHUA_CGI: {
            std::string url = "http://" + info.ip + ":" + std::to_string(info.port) + "/cgi-bin/ptz.cgi?action=stop&channel=0&code=TourStart";
            return !httpGet(url, info.username, info.password).empty();
        }
        default: return stopMove(camera_id);
    }
}

bool PTZManager::addPatrolEntry(int camera_id, int patrol_id, const PTZPatrolEntry& entry) {
    LOG_INFO("[PTZ] AddPatrolEntry cam={} patrol={} preset={}", camera_id, patrol_id, entry.preset_id);
    // Simplified: many cameras require complex XML/JSON to define the whole patrol at once.
    return true; 
}

std::vector<PTZPatrol> PTZManager::getPatrols(int camera_id) {
    // Return a dummy patrol for now
    PTZPatrol p;
    p.id = 1;
    p.name = "Default Patrol";
    return {p};
}

bool PTZManager::savePreset(int camera_id, int preset_id, const std::string& name) {
    LOG_INFO("[PTZ] SavePreset cam={} preset={} name={}", camera_id, preset_id, name);
    auto info = getCameraInfo(camera_id);
    switch (info.protocol) {
        case PTZProtocol::HIKVISION_ISAPI: {
            // ISAPI: PUT /ISAPI/PTZCtrl/channels/1/presets/<id>
            std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                              "/ISAPI/PTZCtrl/channels/1/presets/" + std::to_string(preset_id);
            // ISAPI accepts the same XML-special set; reuse xmlEscape so an
            // operator can name a preset "Lobby & Door 'A'" without producing
            // malformed XML.
            std::string body = "<PTZPreset><id>" + std::to_string(preset_id) + "</id>"
                               "<presetName>" + xmlEscape(name) + "</presetName></PTZPreset>";
            auto resp = httpPut(url, body, info.username, info.password);
            return !resp.empty();
        }
        case PTZProtocol::DAHUA_CGI: {
            // Dahua: setPreset stores current view at preset slot N.
            // /cgi-bin/ptz.cgi?action=start&channel=0&code=SetPreset&arg1=0&arg2=<id>&arg3=0
            // The protocol does NOT carry a name — the name is GUI-side metadata
            // and Dahua stores it locally only. We still log it so future GET
            // can match what the operator typed against the camera's slot.
            std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                              "/cgi-bin/ptz.cgi?action=start&channel=0&code=SetPreset&arg1=0&arg2=" +
                              std::to_string(preset_id) + "&arg3=0";
            return !httpGet(url, info.username, info.password).empty();
        }
        case PTZProtocol::ONVIF:
            return onvifSetPreset(info, preset_id, name);
        default:
            return false;
    }
}

bool PTZManager::deletePreset(int camera_id, int preset_id) {
    LOG_INFO("[PTZ] DeletePreset cam={} preset={}", camera_id, preset_id);
    auto info = getCameraInfo(camera_id);
    switch (info.protocol) {
        case PTZProtocol::HIKVISION_ISAPI: {
            // ISAPI: DELETE /ISAPI/PTZCtrl/channels/1/presets/<id>
            std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                              "/ISAPI/PTZCtrl/channels/1/presets/" + std::to_string(preset_id);
            return httpDelete(url, info.username, info.password);
        }
        case PTZProtocol::DAHUA_CGI: {
            // Dahua CGI uses GET-with-action verbs even for delete:
            // /cgi-bin/ptz.cgi?action=clearPreset&channel=0&arg1=<id>
            std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                              "/cgi-bin/ptz.cgi?action=clearPreset&channel=0&arg1=" +
                              std::to_string(preset_id);
            return !httpGet(url, info.username, info.password).empty();
        }
        case PTZProtocol::ONVIF:
            return onvifRemovePreset(info, preset_id);
        default:
            return false;
    }
}

PTZCapabilities PTZManager::getCapabilities(int camera_id) {
    PTZCapabilities caps;
    caps.can_pan = true;
    caps.can_tilt = true;
    caps.can_zoom = true;
    caps.has_presets = true;
    caps.max_presets = 256;
    return caps;
}

// ============================================================================
// Hikvision ISAPI Implementation
// ============================================================================

bool PTZManager::hikvisionMove(const CameraConnectionInfo& info, float pan, float tilt, float zoom) {
    // PUT /ISAPI/PTZCtrl/channels/1/continuous
    int p = static_cast<int>(pan * 100);
    int t = static_cast<int>(tilt * 100);
    int z = static_cast<int>(zoom * 100);
    
    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                      "/ISAPI/PTZCtrl/channels/1/continuous";
    std::string body = "<PTZData><pan>" + std::to_string(p) + "</pan>"
                       "<tilt>" + std::to_string(t) + "</tilt>"
                       "<zoom>" + std::to_string(z) + "</zoom></PTZData>";
    
    auto resp = httpPut(url, body, info.username, info.password);
    return !resp.empty();
}

bool PTZManager::hikvisionStop(const CameraConnectionInfo& info) {
    return hikvisionMove(info, 0, 0, 0);
}

bool PTZManager::hikvisionGotoPreset(const CameraConnectionInfo& info, int preset_id) {
    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                      "/ISAPI/PTZCtrl/channels/1/presets/" + std::to_string(preset_id) + "/goto";
    std::string body = "<PTZData><PresetID>" + std::to_string(preset_id) + "</PresetID></PTZData>";
    auto resp = httpPut(url, body, info.username, info.password);
    return !resp.empty();
}

std::vector<PTZPreset> PTZManager::hikvisionGetPresets(const CameraConnectionInfo& info) {
    std::vector<PTZPreset> presets;
    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                      "/ISAPI/PTZCtrl/channels/1/presets";
    auto resp = httpGet(url, info.username, info.password);
    
    // Parse simple XML response
    std::regex preset_regex(R"(<id>(\d+)</id>\s*<presetName>([^<]*)</presetName>)");
    auto it = std::sregex_iterator(resp.begin(), resp.end(), preset_regex);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        PTZPreset p;
        p.id = std::stoi((*it)[1].str());
        p.name = (*it)[2].str();
        presets.push_back(p);
    }
    return presets;
}

// ============================================================================
// Dahua CGI Implementation
// ============================================================================

bool PTZManager::dahuaMove(const CameraConnectionInfo& info, float pan, float tilt, float zoom) {
    // Determine direction code
    std::string code;
    int speed = static_cast<int>(std::max(std::abs(pan), std::abs(tilt)) * 8);
    speed = std::max(1, std::min(8, speed));

    if (std::abs(pan) < 0.1f && std::abs(tilt) < 0.1f) {
        if (zoom > 0.1f) code = "ZoomTele";
        else if (zoom < -0.1f) code = "ZoomWide";
        else return dahuaStop(info);
    } else if (pan > 0.1f && tilt > 0.1f) code = "RightUp";
    else if (pan > 0.1f && tilt < -0.1f) code = "RightDown";
    else if (pan < -0.1f && tilt > 0.1f) code = "LeftUp";
    else if (pan < -0.1f && tilt < -0.1f) code = "LeftDown";
    else if (pan > 0.1f) code = "Right";
    else if (pan < -0.1f) code = "Left";
    else if (tilt > 0.1f) code = "Up";
    else if (tilt < -0.1f) code = "Down";
    else return dahuaStop(info);

    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                      "/cgi-bin/ptz.cgi?action=start&channel=0&code=" + code +
                      "&arg1=0&arg2=" + std::to_string(speed) + "&arg3=0";
    auto resp = httpGet(url, info.username, info.password);
    return resp.find("OK") != std::string::npos || resp.find("ok") != std::string::npos || !resp.empty();
}

bool PTZManager::dahuaStop(const CameraConnectionInfo& info) {
    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                      "/cgi-bin/ptz.cgi?action=stop&channel=0&code=Up&arg1=0&arg2=0&arg3=0";
    auto resp = httpGet(url, info.username, info.password);
    return !resp.empty();
}

bool PTZManager::dahuaGotoPreset(const CameraConnectionInfo& info, int preset_id) {
    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                      "/cgi-bin/ptz.cgi?action=start&channel=0&code=GotoPreset&arg1=0&arg2=" + 
                      std::to_string(preset_id) + "&arg3=0";
    auto resp = httpGet(url, info.username, info.password);
    return !resp.empty();
}

std::vector<PTZPreset> PTZManager::dahuaGetPresets(const CameraConnectionInfo& info) {
    std::vector<PTZPreset> presets;
    // Dahua doesn't have a reliable preset list API, return numbered presets
    for (int i = 1; i <= 16; i++) {
        PTZPreset p;
        p.id = i;
        p.name = "Preset " + std::to_string(i);
        presets.push_back(p);
    }
    return presets;
}

// ============================================================================
// ONVIF PTZ Implementation (SOAP)
// ============================================================================

std::string PTZManager::resolveProfileToken(const CameraConnectionInfo& info) {
    // Cache hit path. Empty cached value = "discovery already failed once" —
    // we do not retry on every SOAP call (would cost 1 round-trip per move).
    if (info.camera_id >= 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = profile_token_cache_.find(info.camera_id);
        if (it != profile_token_cache_.end()) {
            return it->second.empty() ? std::string("Profile_1") : it->second;
        }
    }

    const std::string get_profiles_soap =
        R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
  <s:Body>
    <trt:GetProfiles/>
  </s:Body>
</s:Envelope>)";

    const std::string url = "http://" + info.ip + ":" + std::to_string(info.port) + "/onvif/Media";
    const std::string resp = httpPut(url, get_profiles_soap, info.username, info.password,
                                     "application/soap+xml");

    std::string token;
    if (!resp.empty() && resp.find("Fault") == std::string::npos) {
        // Parse the FIRST `token="..."` attribute on a `<...:Profiles ...>` element.
        // We accept any namespace prefix (`trt:`, `tt:`, `wstoken:`...) by anchoring
        // on the local name `Profiles`. Regex is intentionally permissive — full
        // SOAP/XML parsing isn't justified for this single field.
        static const std::regex profile_re(
            R"REGEX(<[^>]*?:?Profiles\b[^>]*?\btoken\s*=\s*"([^"]+)")REGEX",
            std::regex::icase);
        std::smatch m;
        if (std::regex_search(resp, m, profile_re) && m.size() >= 2) {
            token = m[1].str();
        }
    }

    if (info.camera_id >= 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Store empty string if discovery failed so we don't retry. The
        // accessor above maps empty → "Profile_1" fallback.
        profile_token_cache_[info.camera_id] = token;
    }

    if (token.empty()) {
        LOG_WARN("[PTZ] ONVIF GetProfiles failed for cam={} ip={} — falling back to 'Profile_1'",
                 info.camera_id, info.ip);
        return "Profile_1";
    }
    LOG_INFO("[PTZ] ONVIF profile resolved cam={} → '{}'", info.camera_id, token);
    return token;
}

bool PTZManager::onvifMove(const CameraConnectionInfo& info, float pan, float tilt, float zoom) {
    const std::string profile_token = xmlEscape(resolveProfileToken(info));
    // ONVIF ContinuousMove SOAP request
    std::string soap = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl"
            xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <tptz:ContinuousMove>
      <tptz:ProfileToken>)" + profile_token + R"(</tptz:ProfileToken>
      <tptz:Velocity>
        <tt:PanTilt x=")" + std::to_string(pan) + R"(" y=")" + std::to_string(tilt) + R"("/>
        <tt:Zoom x=")" + std::to_string(zoom) + R"("/>
      </tptz:Velocity>
    </tptz:ContinuousMove>
  </s:Body>
</s:Envelope>)";

    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) + "/onvif/PTZ";
    auto resp = httpPut(url, soap, info.username, info.password, "application/soap+xml");
    return resp.find("Fault") == std::string::npos && !resp.empty();
}

bool PTZManager::onvifStop(const CameraConnectionInfo& info) {
    const std::string profile_token = xmlEscape(resolveProfileToken(info));
    std::string soap = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl">
  <s:Body>
    <tptz:Stop>
      <tptz:ProfileToken>)" + profile_token + R"(</tptz:ProfileToken>
      <tptz:PanTilt>true</tptz:PanTilt>
      <tptz:Zoom>true</tptz:Zoom>
    </tptz:Stop>
  </s:Body>
</s:Envelope>)";

    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) + "/onvif/PTZ";
    auto resp = httpPut(url, soap, info.username, info.password, "application/soap+xml");
    return !resp.empty();
}

bool PTZManager::onvifGotoPreset(const CameraConnectionInfo& info, int preset_id) {
    const std::string profile_token = xmlEscape(resolveProfileToken(info));
    std::string soap = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl">
  <s:Body>
    <tptz:GotoPreset>
      <tptz:ProfileToken>)" + profile_token + R"(</tptz:ProfileToken>
      <tptz:PresetToken>)" + std::to_string(preset_id) + R"(</tptz:PresetToken>
    </tptz:GotoPreset>
  </s:Body>
</s:Envelope>)";

    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) + "/onvif/PTZ";
    auto resp = httpPut(url, soap, info.username, info.password, "application/soap+xml");
    return !resp.empty();
}

std::vector<PTZPreset> PTZManager::onvifGetPresets(const CameraConnectionInfo& info) {
    std::vector<PTZPreset> presets;
    // Simplified: return generic presets
    for (int i = 1; i <= 8; i++) {
        PTZPreset p;
        p.id = i;
        p.name = "Preset " + std::to_string(i);
        presets.push_back(p);
    }
    return presets;
}

bool PTZManager::onvifSetPreset(const CameraConnectionInfo& info, int preset_id, const std::string& name) {
    const std::string profile_token = xmlEscape(resolveProfileToken(info));
    // ONVIF PTZ SetPreset — overwrites if PresetToken is supplied, otherwise
    // creates a new one. We always supply the token so the camera-side ID
    // matches our internal numbering.
    std::string soap = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl">
  <s:Body>
    <tptz:SetPreset>
      <tptz:ProfileToken>)" + profile_token + R"(</tptz:ProfileToken>
      <tptz:PresetName>)" + xmlEscape(name) + R"(</tptz:PresetName>
      <tptz:PresetToken>)" + std::to_string(preset_id) + R"(</tptz:PresetToken>
    </tptz:SetPreset>
  </s:Body>
</s:Envelope>)";

    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) + "/onvif/PTZ";
    auto resp = httpPut(url, soap, info.username, info.password, "application/soap+xml");
    return !resp.empty() && resp.find("Fault") == std::string::npos;
}

bool PTZManager::onvifRemovePreset(const CameraConnectionInfo& info, int preset_id) {
    const std::string profile_token = xmlEscape(resolveProfileToken(info));
    std::string soap = R"(<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl">
  <s:Body>
    <tptz:RemovePreset>
      <tptz:ProfileToken>)" + profile_token + R"(</tptz:ProfileToken>
      <tptz:PresetToken>)" + std::to_string(preset_id) + R"(</tptz:PresetToken>
    </tptz:RemovePreset>
  </s:Body>
</s:Envelope>)";

    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) + "/onvif/PTZ";
    auto resp = httpPut(url, soap, info.username, info.password, "application/soap+xml");
    return !resp.empty() && resp.find("Fault") == std::string::npos;
}

// ============================================================================
// HTTP Helpers
// ============================================================================

std::string PTZManager::httpGet(const std::string& url, const std::string& username, const std::string& password) {
    static const std::regex url_regex(R"(^http://([^/:]+)(?::(\d+))(/.*)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, url_regex)) {
        LOG_ERROR("[PTZ-HTTP] Invalid URL '{}'", url);
        return "";
    }

    int port = 80;
    if (match[2].matched) {
        try {
            port = std::stoi(match[2].str());
        } catch (...) {
            LOG_ERROR("[PTZ-HTTP] Invalid port in '{}'", url);
            return "";
        }
    }

    auto response = vms::http::getDigest(match[1].str(), port, match[3].str(), username, password, 3L, 5L);
    if (response.status_code < 0) {
        LOG_ERROR("[PTZ-HTTP] GET failed: {}", response.error);
        return "";
    }
    return response.body;
}

bool PTZManager::httpDelete(const std::string& url,
                            const std::string& username,
                            const std::string& password) {
    static const std::regex url_regex(R"(^http://([^/:]+)(?::(\d+))(/.*)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, url_regex)) {
        LOG_ERROR("[PTZ-HTTP] Invalid URL '{}'", url);
        return false;
    }
    int port = 80;
    if (match[2].matched) {
        try { port = std::stoi(match[2].str()); }
        catch (...) { LOG_ERROR("[PTZ-HTTP] Invalid port in '{}'", url); return false; }
    }
    auto response = vms::http::deleteDigest(match[1].str(), port, match[3].str(),
                                            username, password, 3L, 5L);
    if (response.status_code < 0) {
        LOG_ERROR("[PTZ-HTTP] DELETE failed: {}", response.error);
        return false;
    }
    // Hikvision returns 200 with an XML body on success; treat 2xx as ok.
    return response.status_code >= 200 && response.status_code < 300;
}

std::string PTZManager::httpPut(const std::string& url, const std::string& body,
                                const std::string& username, const std::string& password,
                                const std::string& content_type) {
    static const std::regex url_regex(R"(^http://([^/:]+)(?::(\d+))(/.*)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, url_regex)) {
        LOG_ERROR("[PTZ-HTTP] Invalid URL '{}'", url);
        return "";
    }

    int port = 80;
    if (match[2].matched) {
        try {
            port = std::stoi(match[2].str());
        } catch (...) {
            LOG_ERROR("[PTZ-HTTP] Invalid port in '{}'", url);
            return "";
        }
    }

    auto response = vms::http::putDigest(match[1].str(), port, match[3].str(), username, password,
                                         body, content_type, 3L, 5L);
    if (response.status_code < 0) {
        LOG_ERROR("[PTZ-HTTP] PUT failed: {}", response.error);
        return "";
    }
    return response.body;
}

bool PTZManager::hikvisionStartPatrol(const CameraConnectionInfo& info, int patrol_id) {
    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                      "/ISAPI/PTZCtrl/channels/1/patrols/" + std::to_string(patrol_id) + "/start";
    auto resp = httpPut(url, "", info.username, info.password);
    return !resp.empty();
}

bool PTZManager::dahuaStartPatrol(const CameraConnectionInfo& info, int patrol_id) {
    std::string url = "http://" + info.ip + ":" + std::to_string(info.port) +
                      "/cgi-bin/ptz.cgi?action=start&channel=0&code=TourStart&arg1=0&arg2=" + std::to_string(patrol_id) + "&arg3=0";
    auto resp = httpGet(url, info.username, info.password);
    return !resp.empty();
}

bool PTZManager::onvifStartPatrol(const CameraConnectionInfo& info, int patrol_id) {
    return false;
}

} // namespace core
} // namespace vms
