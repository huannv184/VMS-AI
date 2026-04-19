#include "video/decoder_ffmpeg.h"
#include <iostream>
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/dict.h>
}

namespace video {

FFmpegDecoder::FFmpegDecoder() {
    av_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    frame_counter_ = 0;
    last_width_ = 0;
    last_height_ = 0;
}

FFmpegDecoder::~FFmpegDecoder() {
    close();
    if (av_frame_) av_frame_free(&av_frame_);
    if (packet_) av_packet_free(&packet_);
}

bool FFmpegDecoder::open(const std::string& uri) {
    close();
    
    // ✅ TỐI ƯU RTSP - Giảm packet loss
    AVDictionary* options = nullptr;
    
    // Force TCP transport (quan trọng nhất!)
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "rtsp_flags", "prefer_tcp", 0);
    
    // Buffer và timeout settings
    av_dict_set(&options, "buffer_size", "2048000", 0);      // 2MB buffer
    av_dict_set(&options, "max_delay", "500000", 0);         // 500ms max delay
    // av_dict_set(&options, "stimeout", "5000000", 0);         // 5s timeout (Removed for Windows compatibility)
    
    // Low latency settings
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);
    av_dict_set(&options, "reorder_queue_size", "0", 0);
    
    // Analyze duration
    av_dict_set(&options, "analyzeduration", "1000000", 0);  // 1s
    av_dict_set(&options, "probesize", "1000000", 0);        // 1MB
    
    int ret = avformat_open_input(&fmt_ctx_, uri.c_str(), nullptr, &options);
    av_dict_free(&options);  // ✅ Luôn free options sau khi dùng
    
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[FFmpegDecoder] Failed to open: " << uri 
                  << " Error: " << errbuf << std::endl;
        return false;
    }
    
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        std::cerr << "[FFmpegDecoder] Failed to find stream info" << std::endl;
        avformat_close_input(&fmt_ctx_);
        return false;
    }
    
    video_stream_idx_ = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; i++) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx_ = i;
            break;
        }
    }
    
    if (video_stream_idx_ == -1) {
        std::cerr << "[FFmpegDecoder] No video stream found" << std::endl;
        avformat_close_input(&fmt_ctx_);
        return false;
    }
    
    AVCodecParameters* codecpar = fmt_ctx_->streams[video_stream_idx_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    
    if (!codec) {
        std::cerr << "[FFmpegDecoder] Codec not found" << std::endl;
        avformat_close_input(&fmt_ctx_);
        return false;
    }
    
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::cerr << "[FFmpegDecoder] Failed to allocate codec context" << std::endl;
        avformat_close_input(&fmt_ctx_);
        return false;
    }
    
    if (avcodec_parameters_to_context(codec_ctx_, codecpar) < 0) {
        std::cerr << "[FFmpegDecoder] Failed to copy codec params" << std::endl;
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }
    
    // Set decoder options for low latency
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    
    AVDictionary* codec_options = nullptr;
    av_dict_set(&codec_options, "threads", "auto", 0);
    av_dict_set(&codec_options, "refcounted_frames", "1", 0);
    
    ret = avcodec_open2(codec_ctx_, codec, &codec_options);
    av_dict_free(&codec_options);
    
    if (ret < 0) {
        std::cerr << "[FFmpegDecoder] Failed to open codec" << std::endl;
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }
    
    std::cout << "[FFmpegDecoder] Opened: " << uri 
              << " (" << codec_ctx_->width << "x" << codec_ctx_->height << ")" 
              << std::endl;
    
    return true;
}

// Public API: chuyển đổi edge::Frame -> video::Frame
bool FFmpegDecoder::read(Frame& frame) {
    edge::Frame temp_frame;
    
    if (!readInternal(temp_frame)) {
        return false;
    }
    
    // Convert edge::Frame to video::Frame
    int cv_type = (temp_frame.channels == 1) ? CV_8UC1 : 
                  (temp_frame.channels == 3) ? CV_8UC3 : CV_8UC4;
    
    cv::Mat img(temp_frame.height, temp_frame.width, cv_type, temp_frame.ptr());
    
    frame.image = img.clone();  // Deep copy
    frame.timestamp = temp_frame.pts;
    
    return true;
}

// Internal: Giữ nguyên logic cũ
bool FFmpegDecoder::readInternal(edge::Frame& frame) {
    if (!fmt_ctx_ || !codec_ctx_) {
        return false;
    }
    
    while (true) {
        int ret = av_read_frame(fmt_ctx_, packet_);
        
        if (ret < 0) {
            return false;
        }
        
        if (packet_->stream_index != video_stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }
        
        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        
        if (ret < 0) {
            continue;
        }
        
        ret = avcodec_receive_frame(codec_ctx_, av_frame_);
        
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            continue;
        } else if (ret < 0) {
            return false;
        }
        
        // Convert to BGR24
        // FIX: Check if resolution changed or context missing
        if (!sws_ctx_ || 
            codec_ctx_->width != last_width_ || 
            codec_ctx_->height != last_height_) {
            
            if (sws_ctx_) sws_freeContext(sws_ctx_);
            
            sws_ctx_ = sws_getContext(
                codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
                codec_ctx_->width, codec_ctx_->height, AV_PIX_FMT_BGR24,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            last_width_ = codec_ctx_->width;
            last_height_ = codec_ctx_->height;
        }
        
        // Fill edge::Frame
        frame.width = codec_ctx_->width;
        frame.height = codec_ctx_->height;
        frame.channels = 3;
        frame.pts = av_frame_->pts;
        frame.cam_id = 0;
        frame.data.resize(frame.width * frame.height * frame.channels);
        
        uint8_t* dest[1] = { frame.ptr() };
        int dest_linesize[1] = { static_cast<int>(frame.width * 3) };
        
        sws_scale(sws_ctx_, av_frame_->data, av_frame_->linesize,
                  0, codec_ctx_->height, dest, dest_linesize);
        
        frame_counter_++;
        return true;
    }
}

void FFmpegDecoder::close() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
    
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
    }
    
    frame_counter_ = 0;
    video_stream_idx_ = -1;
}

} // namespace video