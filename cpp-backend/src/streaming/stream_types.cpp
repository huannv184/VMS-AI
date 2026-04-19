// ==============================================================
// File: src/streaming/stream_types.cpp
// Multi-Stream Type Implementations
// ==============================================================

#include "streaming/stream_types.h"
#include <sstream>
#include <algorithm>
#include <regex>

namespace vms::streaming {

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

const char* streamTypeToString(StreamType type) {
    switch (type) {
        case StreamType::MAIN: return "main";
        case StreamType::SUB: return "sub";
        case StreamType::THIRD: return "third";
        default: return "unknown";
    }
}

StreamType stringToStreamType(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "main") return StreamType::MAIN;
    if (lower == "sub") return StreamType::SUB;
    if (lower == "third") return StreamType::THIRD;
    
    return StreamType::MAIN; // Default
}

// ============================================================================
// STREAM URL BUILDER
// ============================================================================

std::string StreamURLBuilder::build(
    const std::string& brand,
    const std::string& ip,
    const std::string& username,
    const std::string& password,
    int port,
    StreamType type
) {
    std::string lower_brand = brand;
    std::transform(lower_brand.begin(), lower_brand.end(), lower_brand.begin(), ::tolower);
    
    if (lower_brand == "hikvision") {
        return buildHikvision(ip, username, password, port, type);
    } else if (lower_brand == "dahua") {
        return buildDahua(ip, username, password, port, type);
    } else if (lower_brand == "axis") {
        return buildAxis(ip, username, password, port, type);
    } else {
        return buildGeneric(ip, username, password, port, type);
    }
}

std::string StreamURLBuilder::buildHikvision(
    const std::string& ip,
    const std::string& user,
    const std::string& pass,
    int port,
    StreamType type
) {
    // Hikvision format: rtsp://user:pass@ip:port/Streaming/Channels/{channel}
    // Main: 101, Sub: 102, Third: 103
    int channel = 101;
    switch (type) {
        case StreamType::MAIN:  channel = 101; break;
        case StreamType::SUB:   channel = 102; break;
        case StreamType::THIRD: channel = 103; break;
    }
    
    std::stringstream ss;
    ss << "rtsp://" << user << ":" << pass << "@" << ip << ":" << port
       << "/Streaming/Channels/" << channel;
    
    return ss.str();
}

std::string StreamURLBuilder::buildDahua(
    const std::string& ip,
    const std::string& user,
    const std::string& pass,
    int port,
    StreamType type
) {
    // Dahua format: rtsp://user:pass@ip:port/cam/realmonitor?channel=1&subtype={n}
    // Main: 0, Sub: 1, Third: 2
    int subtype = 0;
    switch (type) {
        case StreamType::MAIN:  subtype = 0; break;
        case StreamType::SUB:   subtype = 1; break;
        case StreamType::THIRD: subtype = 2; break;
    }
    
    std::stringstream ss;
    ss << "rtsp://" << user << ":" << pass << "@" << ip << ":" << port
       << "/cam/realmonitor?channel=1&subtype=" << subtype;
    
    if (type == StreamType::SUB || type == StreamType::THIRD) {
        ss << "&tcp"; // Force TCP for sub/third streams
    }
    
    return ss.str();
}

std::string StreamURLBuilder::buildAxis(
    const std::string& ip,
    const std::string& user,
    const std::string& pass,
    int port,
    StreamType type
) {
    // Axis format: rtsp://user:pass@ip/axis-media/media.amp?videocodec=h264&resolution={w}x{h}
    std::string resolution = "1920x1080";
    switch (type) {
        case StreamType::MAIN:  resolution = "1920x1080"; break;
        case StreamType::SUB:   resolution = "640x360"; break;
        case StreamType::THIRD: resolution = "640x360"; break;
    }
    
    std::stringstream ss;
    ss << "rtsp://" << user << ":" << pass << "@" << ip << ":" << port
       << "/axis-media/media.amp?videocodec=h264&resolution=" << resolution;
    
    return ss.str();
}

std::string StreamURLBuilder::buildGeneric(
    const std::string& ip,
    const std::string& user,
    const std::string& pass,
    int port,
    StreamType type
) {
    // Generic ONVIF-like format: rtsp://user:pass@ip:port/stream{n}
    int stream_num = 1;
    switch (type) {
        case StreamType::MAIN:  stream_num = 1; break;
        case StreamType::SUB:   stream_num = 2; break;
        case StreamType::THIRD: stream_num = 3; break;
    }
    
    std::stringstream ss;
    ss << "rtsp://" << user << ":" << pass << "@" << ip << ":" << port
       << "/stream" << stream_num;
    
    return ss.str();
}

std::string StreamURLBuilder::detectBrand(const std::string& rtsp_url) {
    std::string lower_url = rtsp_url;
    std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), ::tolower);
    
    if (lower_url.find("/streaming/channels/") != std::string::npos) {
        return "hikvision";
    } else if (lower_url.find("/cam/realmonitor") != std::string::npos) {
        return "dahua";
    } else if (lower_url.find("/axis-media/") != std::string::npos) {
        return "axis";
    }
    
    return "generic";
}

bool StreamURLBuilder::validateURL(const std::string& url) {
    // Basic RTSP URL validation
    if (url.empty()) return false;
    
    // Must start with rtsp://
    if (url.substr(0, 7) != "rtsp://") return false;
    
    // Must contain @ (credentials separator)
    if (url.find('@') == std::string::npos) return false;
    
    // Simple regex validation
    std::regex rtsp_regex(R"(rtsp://[^:]+:[^@]+@[^:/]+:\d+/.+)");
    return std::regex_match(url, rtsp_regex);
}

} // namespace vms::streaming
