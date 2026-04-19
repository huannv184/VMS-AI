#pragma once
#include <string>
#include "video_types.h"

namespace video {

struct SourceConfig {
    SourceType type{SourceType::FILE};
    std::string uri;
    int fps{30};
    int width{1920};
    int height{1080};
};

} // namespace video
