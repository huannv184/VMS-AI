// D:\buidC\ai2.1\video\src\video_source_factory.cpp
#include "video/video_source_factory.h"
#include "video/source_rtsp.h"
#include <iostream>

namespace video {

std::unique_ptr<VideoSource> VideoSourceFactory::create(const SourceConfig& cfg) {
    switch (cfg.type) {
        case SourceType::RTSP: {
            std::cout << "Creating RTSP source: " << cfg.uri << std::endl;
            return std::make_unique<RtspSource>(cfg.uri);
        }
        
        case SourceType::FILE: {
            std::cout << "Creating FILE source: " << cfg.uri << std::endl;
            return std::make_unique<RtspSource>(cfg.uri);  // FFmpeg decoder hỗ trợ cả file
        }
        
        case SourceType::CAMERA: {
            std::cout << "Creating CAMERA source: " << cfg.uri << std::endl;
            // Camera cũng có thể dùng FFmpeg với URI dạng: video=<device_name>
            std::string camera_uri = "video=" + cfg.uri;
            return std::make_unique<RtspSource>(camera_uri);
        }
        
        default:
            std::cerr << "Unknown source type" << std::endl;
            return nullptr;
    }
}

} // namespace video