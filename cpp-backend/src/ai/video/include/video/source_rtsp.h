#pragma once

#include <string>
#include "video_source.h"      // VideoSource đã include infer_types.h
#include "decoder_ffmpeg.h"    // FFmpegDecoder đã include infer_types.h

namespace video {

class RtspSource : public VideoSource {
public:
    explicit RtspSource(const std::string& uri);

    bool open() override;
    bool read(Frame& frame) override;  // video::Frame từ infer_types.h
    void close() override;

private:
    std::string uri_;
    FFmpegDecoder decoder_;
};

} // namespace video