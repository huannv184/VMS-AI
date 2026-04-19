#include "video_pipeline.h"

namespace video {

VideoPipeline::VideoPipeline(std::unique_ptr<VideoSource> source)
    : source_(std::move(source)) {}

bool VideoPipeline::open() {
    return source_ && source_->open();
}

bool VideoPipeline::read(video::Frame& frame) {
    return source_ && source_->read(frame);
}

void VideoPipeline::close() {
    if (source_) source_->close();
}

} // namespace video
