#pragma once

#include "common/frame.h"  // edge::Frame
#include "inference/infer_types.h"  // video::Frame
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace video {

class FFmpegDecoder {
public:
    FFmpegDecoder();
    ~FFmpegDecoder();

    bool open(const std::string& uri);
    
    // Public API nhận video::Frame (cv::Mat)
    bool read(Frame& frame);
    
    void close();

private:
    // Internal method dùng edge::Frame
    bool readInternal(edge::Frame& frame);
    
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    AVFrame* av_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    uint32_t frame_counter_ = 0;
    
    // Resolution change tracking
    int last_width_ = 0;
    int last_height_ = 0;
};

} // namespace video