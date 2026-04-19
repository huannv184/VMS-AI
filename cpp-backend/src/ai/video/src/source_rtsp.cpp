#include "video/source_rtsp.h"

namespace video {

RtspSource::RtspSource(const std::string& uri) : uri_(uri) {}

bool RtspSource::open() {
    return decoder_.open(uri_);
}

bool RtspSource::read(video::Frame& frame) {
    return decoder_.read(frame);
}

void RtspSource::close() {
    decoder_.close();
}

} // namespace video
