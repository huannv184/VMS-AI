#include "video_pipeline.h"

namespace video {

VideoPipeline::VideoPipeline(std::unique_ptr<VideoSource> source)
    : source_(std::move(source))
    , running_(false)
{
}

bool VideoPipeline::open() {
    if (!source_) {
        return false;
    }
    
    if (source_->open()) {
        running_ = true;
        return true;
    }
    
    return false;
}

void VideoPipeline::close() {
    if (source_) {
        source_->close();
    }
    running_ = false;
}

bool VideoPipeline::read(Frame& frame) {
    if (!source_) return false;
    if (!running_) return false;

    // Đọc frame từ nguồn
    if (!source_->read(frame)) {
        return false;
    }

    // Push frame đến các sink (nếu có)
    for (auto* sink : sinks_) {
        if (sink) {
            Frame tmp = frame; // copy để không ảnh hưởng frame gốc
            sink->read(tmp);
        }
    }

    return true;
}

} // namespace video