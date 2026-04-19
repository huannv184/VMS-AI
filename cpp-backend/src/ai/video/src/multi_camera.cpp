#include "video/multi_camera.h"

namespace video {

void MultiCamera::add(std::unique_ptr<VideoPipeline> pipe) {
    pipes_.push_back(std::move(pipe));
}

void MultiCamera::openAll() {
    for (auto& p : pipes_)
        p->open();
}

} // namespace video
