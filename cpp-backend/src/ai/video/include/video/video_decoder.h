#pragma once
#include <string>
#include "inference/infer_types.h"  // video::Frame

namespace video {

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    virtual bool open(const std::string& uri) = 0;
    virtual bool read(video::Frame& frame) = 0;
    virtual void close() = 0;
};

} // namespace video
