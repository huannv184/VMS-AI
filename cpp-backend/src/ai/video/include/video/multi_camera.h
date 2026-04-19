#pragma once
#include <vector>
#include <memory>
#include "video_pipeline.h"

namespace video {

class MultiCamera {
public:
    void add(std::unique_ptr<VideoPipeline> pipe);
    void openAll();

private:
    std::vector<std::unique_ptr<VideoPipeline>> pipes_;
};

} // namespace video
