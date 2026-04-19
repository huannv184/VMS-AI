#pragma once

#include <string>
#include "video_source.h"
#include "decoder_ffmpeg.h"

namespace video {

class RtspReader : public VideoSource {
public:
    explicit RtspReader(const std::string& uri);
    ~RtspReader();

    bool open() override;
    bool read(Frame& frame) override;
    void close() override;

private:
    std::string uri_;
    FFmpegDecoder decoder_;
};

} // namespace video