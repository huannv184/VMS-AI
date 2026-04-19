#pragma once

#include "inference/infer_types.h"  // ← Dùng video::Frame

namespace video {

class VideoSource {
public:
    virtual ~VideoSource() = default;

    virtual bool open() = 0;
    virtual bool read(Frame& frame) = 0;
    virtual void close() {}
};

} // namespace video