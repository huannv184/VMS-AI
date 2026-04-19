#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace CameraDiscovery {

enum class Brand {
    Unknown,
    Hikvision, Dahua, ONVIF, Axis, Hanwha, Bosch, Vivotek, Milesight, Reolink, CPPlus,
    // --- Hikvision OEM ---
    Annke, LTS, LaView, Swann, Trendnet, Epcom, GWSecurity, Hunt, WBox, Ezviz,
    // --- Dahua OEM ---
    Amcrest, Lorex, QSee, ICRealtime, Honeywell, Panasonic, FLIR, Tyco, Imou, Kbvision, Vantech, Questek,
    // --- Uniview OEM ---
    Uniview, Foscam, Pelco, GeoVision, Luma, Speco
};

struct CameraChannel {
    int         channel_id = 0;
    std::string name;
    std::string rtsp_main;
    std::string rtsp_sub;
    std::string resolution;
    std::string codec;
    bool        online = false;
};

struct DeviceInfo {
    Brand       brand = Brand::Unknown;
    std::string model;
    std::string serial;
    std::string firmware;
    int         channels = 0;
    std::string error;
    std::string raw_advanced_config; // Store raw JSON/XML from manufacturer API
    std::vector<CameraChannel> cameras;
};

struct DiscoveryConfig {
    std::string host;
    std::string username;
    std::string password;
    int  http_port   = 80;
    int  rtsp_port   = 554;
    int  timeout_sec = 5;
    Brand brand = Brand::Unknown;
};

inline std::string brandToString(Brand b) {
    switch (b) {
        case Brand::Hikvision: return "Hikvision";
        case Brand::Dahua:     return "Dahua";
        case Brand::ONVIF:     return "ONVIF";
        case Brand::Axis:      return "Axis";
        case Brand::Hanwha:    return "Hanwha";
        case Brand::Bosch:     return "Bosch";
        case Brand::Vivotek:   return "Vivotek";
        case Brand::Milesight: return "Milesight";
        case Brand::Reolink:   return "Reolink";
        case Brand::CPPlus:    return "CP Plus";
        case Brand::Uniview:   return "Uniview";
        case Brand::Foscam:    return "Foscam";
        case Brand::Pelco:     return "Pelco";
        case Brand::Kbvision:  return "Kbvision";
        default:               return "Unknown";
    }
}

} // namespace CameraDiscovery
