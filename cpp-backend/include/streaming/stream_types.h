// ==============================================================
// File: include/streaming/stream_types.h
// Multi-Stream Type Definitions & Utilities
// ==============================================================

#pragma once

#include <string>
#include <cstdint>

namespace vms {
namespace core {
    enum class PipelineState {
        STOPPED,      // Initial state, not running
        CONNECTING,   // Starting FFmpeg, establishing RTSP connection
        RUNNING,      // Normal operation, healthy
        DEGRADED,     // Running but with issues (low FPS, errors)
        FAILING,      // Critical errors, needs restart
        STOPPING,     // Graceful shutdown in progress
        RESTARTING    // Automatic restart in progress
    };
}
}

namespace vms::streaming {

// ============================================================================
// STREAM TYPES
// ============================================================================

enum class StreamType {
    MAIN,   // High-res recording (1080p, 25fps)
    SUB,    // Web streaming (360p-720p, 15fps)
    THIRD   // AI processing (640x360, 15fps)
};

const char* streamTypeToString(StreamType type);
StreamType stringToStreamType(const std::string& str);

// ============================================================================
// STREAM PROFILE
// ============================================================================

struct StreamProfile {
    StreamType type{StreamType::MAIN};
    std::string rtsp_url;
    
    // Expected characteristics
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t fps{25};
    std::string codec{"h264"};
    
    // Purpose flags
    bool enable_recording{false};
    bool enable_streaming{false};
    bool enable_ai{false};
    
    // Quality
    uint32_t bitrate_kbps{4000};
    int jpeg_quality{75}; // For streaming (0-100)
    
    // Recording (for MAIN stream)
    std::string recording_path;
    int segment_duration{300}; // seconds
    
    // Validation
    bool validate() const {
        if (rtsp_url.empty()) return false;
        if (width == 0 || height == 0 || fps == 0) return false;
        if (jpeg_quality < 1 || jpeg_quality > 100) return false;
        return true;
    }
    
    // Defaults by type
    static StreamProfile createMain(const std::string& url) {
        StreamProfile profile;
        profile.type = StreamType::MAIN;
        profile.rtsp_url = url;
        profile.width = 1920;
        profile.height = 1080;
        profile.fps = 25;
        profile.enable_recording = true;
        profile.recording_path = "data/record";
        return profile;
    }
    
    static StreamProfile createSub(const std::string& url) {
        StreamProfile profile;
        profile.type = StreamType::SUB;
        profile.rtsp_url = url;
        profile.width = 640;
        profile.height = 360;
        profile.fps = 15;
        profile.enable_streaming = true;
        profile.jpeg_quality = 70;
        return profile;
    }
    
    static StreamProfile createThird(const std::string& url) {
        StreamProfile profile;
        profile.type = StreamType::THIRD;
        profile.rtsp_url = url;
        profile.width = 640;
        profile.height = 360;
        profile.fps = 15;
        profile.enable_ai = true;
        return profile;
    }
};

// ============================================================================
// CAMERA STREAM CONFIGURATION
// ============================================================================

struct CameraStreamConfig {
    int camera_id{0};
    std::string name;
    std::string brand; // "hikvision", "dahua", "axis", "generic"
    
    // Connection info
    std::string ip_address;
    int rtsp_port{554};
    std::string username;
    std::string password;
    
    // Stream profiles
    StreamProfile main;
    StreamProfile sub;
    StreamProfile third;
    
    // Active streams bitmask (bit 0=main, 1=sub, 2=third)
    uint8_t active_streams{0b111}; // All enabled by default
    
    bool isMainEnabled() const { return (active_streams & 0b001) != 0; }
    bool isSubEnabled() const { return (active_streams & 0b010) != 0; }
    bool isThirdEnabled() const { return (active_streams & 0b100) != 0; }
    
    void enableMain(bool enable) {
        if (enable) active_streams |= 0b001;
        else active_streams &= ~0b001;
    }
    
    void enableSub(bool enable) {
        if (enable) active_streams |= 0b010;
        else active_streams &= ~0b010;
    }
    
    void enableThird(bool enable) {
        if (enable) active_streams |= 0b100;
        else active_streams &= ~0b100;
    }
};

// ============================================================================
// STREAM URL BUILDER
// ============================================================================

class StreamURLBuilder {
public:
    // Build RTSP URL for specific camera brand and stream type
    static std::string build(
        const std::string& brand,
        const std::string& ip,
        const std::string& username,
        const std::string& password,
        int port,
        StreamType type
    );
    
    // Brand-specific builders
    static std::string buildHikvision(const std::string& ip, const std::string& user, 
                                     const std::string& pass, int port, StreamType type);
    
    static std::string buildDahua(const std::string& ip, const std::string& user,
                                 const std::string& pass, int port, StreamType type);
    
    static std::string buildAxis(const std::string& ip, const std::string& user,
                                const std::string& pass, int port, StreamType type);
    
    static std::string buildGeneric(const std::string& ip, const std::string& user,
                                   const std::string& pass, int port, StreamType type);
    
    // Parse brand from URL or detect
    static std::string detectBrand(const std::string& rtsp_url);
    
    // Validate RTSP URL format
    static bool validateURL(const std::string& url);
};

} // namespace vms::streaming
