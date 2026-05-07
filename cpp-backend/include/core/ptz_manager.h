#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

namespace vms {
namespace core {

struct PTZPreset {
    int id{0};
    std::string name;
    float pan{0.0f};
    float tilt{0.0f};
    float zoom{0.0f};
};

struct PTZPatrolEntry {
    int preset_id;
    int stay_time_sec{10};
    int speed{50}; // 1-100
};

struct PTZPatrol {
    int id{0};
    std::string name;
    std::vector<PTZPatrolEntry> entries;
    bool is_active{false};
};

struct PTZCapabilities {
    bool can_pan{false};
    bool can_tilt{false};
    bool can_zoom{false};
    bool has_presets{false};
    int max_presets{0};
    float pan_range_min{-1.0f};
    float pan_range_max{1.0f};
    float tilt_range_min{-1.0f};
    float tilt_range_max{1.0f};
    float zoom_range_min{0.0f};
    float zoom_range_max{1.0f};
};

enum class PTZProtocol {
    ONVIF,
    HIKVISION_ISAPI,
    DAHUA_CGI,
    UNKNOWN
};

/**
 * @brief PTZ Manager - Controls Pan/Tilt/Zoom for cameras
 * 
 * Supports ONVIF SOAP, Hikvision ISAPI, and Dahua CGI protocols.
 * Auto-detects protocol based on camera brand stored in DB.
 */
class PTZManager {
public:
    static PTZManager& getInstance();

    // Continuous movement (-1.0 to 1.0 speed)
    bool continuousMove(int camera_id, float pan_speed, float tilt_speed, float zoom_speed);
    bool stopMove(int camera_id);

    // Absolute positioning (normalized 0.0-1.0)
    bool absoluteMove(int camera_id, float pan, float tilt, float zoom);

    // Relative movement
    bool relativeMove(int camera_id, float pan_delta, float tilt_delta, float zoom_delta);

    // Presets
    std::vector<PTZPreset> getPresets(int camera_id);
    bool gotoPreset(int camera_id, int preset_id);
    bool savePreset(int camera_id, int preset_id, const std::string& name);
    bool deletePreset(int camera_id, int preset_id);

    // Patrols (Tours)
    bool startPatrol(int camera_id, int patrol_id);
    bool stopPatrol(int camera_id);
    bool addPatrolEntry(int camera_id, int patrol_id, const PTZPatrolEntry& entry);
    std::vector<PTZPatrol> getPatrols(int camera_id);

    // Capabilities
    PTZCapabilities getCapabilities(int camera_id);

private:
    PTZManager() = default;
    ~PTZManager() = default;
    PTZManager(const PTZManager&) = delete;
    PTZManager& operator=(const PTZManager&) = delete;

    // Protocol detection
    PTZProtocol detectProtocol(int camera_id);
    
    struct CameraConnectionInfo {
        int camera_id{-1};
        std::string ip;
        int port{80};
        std::string username;
        std::string password;
        std::string brand;
        PTZProtocol protocol{PTZProtocol::UNKNOWN};
    };
    CameraConnectionInfo getCameraInfo(int camera_id);

    // Protocol-specific implementations
    bool hikvisionMove(const CameraConnectionInfo& info, float pan, float tilt, float zoom);
    bool hikvisionStop(const CameraConnectionInfo& info);
    bool hikvisionGotoPreset(const CameraConnectionInfo& info, int preset_id);
    std::vector<PTZPreset> hikvisionGetPresets(const CameraConnectionInfo& info);

    bool dahuaMove(const CameraConnectionInfo& info, float pan, float tilt, float zoom);
    bool dahuaStop(const CameraConnectionInfo& info);
    bool dahuaGotoPreset(const CameraConnectionInfo& info, int preset_id);
    std::vector<PTZPreset> dahuaGetPresets(const CameraConnectionInfo& info);

    bool onvifMove(const CameraConnectionInfo& info, float pan, float tilt, float zoom);
    bool onvifStop(const CameraConnectionInfo& info);
    bool onvifGotoPreset(const CameraConnectionInfo& info, int preset_id);
    std::vector<PTZPreset> onvifGetPresets(const CameraConnectionInfo& info);
    bool onvifSetPreset(const CameraConnectionInfo& info, int preset_id, const std::string& name);
    bool onvifRemovePreset(const CameraConnectionInfo& info, int preset_id);

    // Resolve the ONVIF MediaProfile token for this camera. Looks up a cached
    // token first; on miss issues a Media GetProfiles SOAP call and stores the
    // first profile's token. Falls back to "Profile_1" so existing devices
    // (which advertise that exact token) keep working when discovery fails.
    std::string resolveProfileToken(const CameraConnectionInfo& info);
    
    bool hikvisionStartPatrol(const CameraConnectionInfo& info, int patrol_id);
    bool dahuaStartPatrol(const CameraConnectionInfo& info, int patrol_id);
    bool onvifStartPatrol(const CameraConnectionInfo& info, int patrol_id);

    // HTTP helper
    std::string httpGet(const std::string& url, const std::string& username, const std::string& password);
    std::string httpPut(const std::string& url, const std::string& body,
                        const std::string& username, const std::string& password,
                        const std::string& content_type = "application/xml");
    bool httpDelete(const std::string& url, const std::string& username, const std::string& password);

    std::mutex mutex_;
    // Cache protocol per camera to avoid repeated detection
    std::unordered_map<int, PTZProtocol> protocol_cache_;
    // Cache resolved ONVIF MediaProfile token per camera. Empty string means
    // "tried discovery and failed — caller should use legacy fallback".
    std::unordered_map<int, std::string> profile_token_cache_;
};

} // namespace core
} // namespace vms
