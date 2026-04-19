#include "video/rtsp_reader.h"

namespace video {

RtspReader::RtspReader(const std::string& uri)
    : uri_(uri) {}

RtspReader::~RtspReader() {
    close();
}

bool RtspReader::open() {
    return decoder_.open(uri_);
}

bool RtspReader::read(Frame& frame) {
    return decoder_.read(frame);
}

void RtspReader::close() {
    decoder_.close();
}

} // namespace video
