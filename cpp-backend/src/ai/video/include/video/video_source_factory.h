#pragma once
#include <memory>
#include "video_source.h"
#include "video_source_config.h"  // SourceConfig

namespace video {

class VideoSourceFactory {
public:
    static std::unique_ptr<VideoSource> create(const SourceConfig& cfg);
};

} // namespace video
