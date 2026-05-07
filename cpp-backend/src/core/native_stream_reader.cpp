#include "core/native_stream_reader.h"
#include "utils/logger.h"
#include <chrono>
#include <algorithm>
#include <vector>

namespace vms {
namespace core {

// Interrupt callback: aborts av_read_frame if blocked > 2 seconds.
int NativeStreamReader::interruptCallback(void* opaque) {
    auto* self = static_cast<NativeStreamReader*>(opaque);
    if (!self) return 1;
    if (self->abort_requested_.load()) return 1;
    auto elapsed = std::chrono::steady_clock::now() - self->last_read_start_;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 10000) {
        LOG_WARN("NativeReader: Interrupt triggered - blocked for > 10000ms");
        return 1; 
    }
    return 0; 
}

NativeStreamReader::NativeStreamReader() {
    avformat_network_init();
}

NativeStreamReader::~NativeStreamReader() {
    close();
}

bool NativeStreamReader::open(const std::string& url) {
    url_ = url;
    abort_requested_ = false;
    first_frame_decoded_ = false;
    consecutive_timeouts_ = 0;

    AVDictionary* options = nullptr;
    // --- ULTRA LOW LATENCY & HARDWARE ACCEL PREP ---
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "3000000", 0);             // 3s socket timeout (µs)
    av_dict_set(&options, "probesize", "65536", 0);              // minimal probe → fast open
    av_dict_set(&options, "analyzeduration", "200000", 0);       // 200ms → fast stream info
    av_dict_set(&options, "fflags", "nobuffer+discardcorrupt", 0);
    av_dict_set(&options, "flags", "low_delay", 0);
    av_dict_set(&options, "allowed_media_types", "video", 0);    // Skip audio entirely
    av_dict_set(&options, "buffer_size", "4194304", 0);          // 4MB buffer for 6MP/4K stability
    av_dict_set(&options, "reorder_queue_size", "0", 0);         // disable PTS reorder buffer

    fmt_ctx_ = avformat_alloc_context();
    last_read_start_ = std::chrono::steady_clock::now();
    fmt_ctx_->interrupt_callback.callback = &NativeStreamReader::interruptCallback;
    fmt_ctx_->interrupt_callback.opaque = this;

    if (avformat_open_input(&fmt_ctx_, url.c_str(), nullptr, &options) < 0) {
        av_dict_free(&options);
        return false;
    }
    av_dict_free(&options);

    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        cleanup();
        return false;
    }

    video_stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        cleanup();
        return false;
    }

    AVStream* stream = fmt_ctx_->streams[video_stream_index_];

    // --- GPU DECODER DISCOVERY ---
    // Try NVIDIA hardware decoder (h264_cuvid) first, then fallback to software
    const AVCodec* decoder = nullptr;
    if (stream->codecpar->codec_id == AV_CODEC_ID_H264) {
        decoder = avcodec_find_decoder_by_name("h264_cuvid");
        if (decoder) LOG_INFO("NativeReader: 🚀 Kích hoạt NVIDIA GPU Decoder (H264 NVDEC) cho '{}'", url);
    } else if (stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        decoder = avcodec_find_decoder_by_name("hevc_cuvid");
        if (decoder) LOG_INFO("NativeReader: 🚀 Kích hoạt NVIDIA GPU Decoder (HEVC NVDEC) cho '{}'", url);
    }

    if (!decoder) {
        decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        LOG_INFO("NativeReader: Sử dụng Software Decoder (CPU) cho '{}'", url);
    }

    codec_ctx_ = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(codec_ctx_, stream->codecpar);

    // Optimized threading: FF_THREAD_SLICE avoids frame-batch bursting.
    // thread_count=2 caps parallelism so decoder outputs frames steadily.
    // GPU decoders (cuvid) manage their own threads internally → force 1.
    if (!decoder || std::string(decoder->name).find("cuvid") == std::string::npos) {
        codec_ctx_->thread_count = 2;
        codec_ctx_->thread_type  = FF_THREAD_SLICE;
    } else {
        codec_ctx_->thread_count = 1;
    }
    
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->err_recognition = 0;

    if (avcodec_open2(codec_ctx_, decoder, nullptr) < 0) {
        cleanup();
        return false;
    }

    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;
    fps_ = (stream->avg_frame_rate.den != 0) ? av_q2d(stream->avg_frame_rate) : 25.0;
    if (fps_ <= 0.0 || fps_ > 120.0) fps_ = 25.0;

    frame_ = av_frame_alloc();
    frame_bgr_ = av_frame_alloc();
    frame_tmp_ = av_frame_alloc();  // scratch: avoids per-frame alloc in readFrame()
    packet_ = av_packet_alloc();

    connected_ = true;
    LOG_INFO("NativeReader: High-Perf Pipeline mở '{}' ({}x{} @ {:.2f} FPS)", url, width_, height_, fps_);
    return true;
}

void NativeStreamReader::close() {
    abort_requested_ = true;
    connected_ = false;
    cleanup();
}

void NativeStreamReader::abort() {
    abort_requested_ = true;
}

void NativeStreamReader::cleanup() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (codec_ctx_) { avcodec_free_context(&codec_ctx_); codec_ctx_ = nullptr; }
    if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); fmt_ctx_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (frame_bgr_) { av_frame_free(&frame_bgr_); frame_bgr_ = nullptr; }
    if (frame_tmp_) { av_frame_free(&frame_tmp_); frame_tmp_ = nullptr; }
    if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
}

bool NativeStreamReader::readFrame(cv::Mat& out_mat, bool skip_decode, int target_width, int target_height) {
    if (!connected_) return false;
    if (target_width <= 0) target_width = codec_ctx_->width;
    if (target_height <= 0) target_height = codec_ctx_->height;

    last_read_start_ = std::chrono::steady_clock::now();

    int ret;
    while (connected_ && !abort_requested_.load()) {
        ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) connected_ = false;
            else if (++consecutive_timeouts_ >= 3) connected_ = false;
            break;
        }

        last_read_start_ = std::chrono::steady_clock::now();
        consecutive_timeouts_ = 0;

        if (packet_->stream_index == video_stream_index_) {
            if (packet_callback_) {
                bool is_key = (packet_->flags & AV_PKT_FLAG_KEY);
                packet_callback_(packet_->data, packet_->size, is_key);
            }

            ret = avcodec_send_packet(codec_ctx_, packet_);
            while (ret >= 0 || ret == AVERROR(EAGAIN)) {
                int recv_ret = avcodec_receive_frame(codec_ctx_, frame_);
                if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) break;
                else if (recv_ret < 0) { av_packet_unref(packet_); return false; }

                if (ret == AVERROR(EAGAIN)) ret = avcodec_send_packet(codec_ctx_, packet_);
                if (frame_->decode_error_flags != 0) continue;

                if (skip_decode) {
                    while (avcodec_receive_frame(codec_ctx_, frame_) >= 0) {} 
                    av_packet_unref(packet_);
                    return true;
                }

                // Drain remaining buffered frames, keep the latest.
                // frame_tmp_ is pre-allocated — no per-frame heap alloc.
                while (avcodec_receive_frame(codec_ctx_, frame_tmp_) >= 0) {
                    av_frame_unref(frame_);
                    av_frame_move_ref(frame_, frame_tmp_);
                }

                // Recreate sws_ctx only when output dimensions or source format changes.
                // Critically: do NOT check out_mat dimensions here — the caller keeps the Mat
                // allocated between frames so cols/rows stay at target values after the first
                // decode. Checking out_mat would recreate sws_ctx on every call if the caller
                // ever reset the Mat (sws_getContext costs ~30ms for large source frames).
                if (!sws_ctx_ || sws_src_w_ != codec_ctx_->width || sws_src_h_ != codec_ctx_->height
                    || sws_src_fmt_ != codec_ctx_->pix_fmt
                    || sws_dst_w_ != target_width || sws_dst_h_ != target_height) {
                    if (sws_ctx_) sws_freeContext(sws_ctx_);
                    sws_ctx_ = sws_getContext(codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
                                             target_width, target_height, AV_PIX_FMT_BGR24,
                                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    sws_src_w_ = codec_ctx_->width;
                    sws_src_h_ = codec_ctx_->height;
                    sws_src_fmt_ = codec_ctx_->pix_fmt;
                    sws_dst_w_ = target_width;
                    sws_dst_h_ = target_height;
                }

                if (out_mat.empty() || out_mat.rows != target_height || out_mat.cols != target_width)
                    out_mat = cv::Mat(target_height, target_width, CV_8UC3);

                uint8_t* dest[4] = { out_mat.data, nullptr, nullptr, nullptr };
                int dest_ls[4] = { static_cast<int>(out_mat.step), 0, 0, 0 };
                sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, codec_ctx_->height, dest, dest_ls);

                if (!first_frame_decoded_) {
                    first_frame_decoded_ = true;
                    LOG_INFO("NativeReader: First frame decoded for '{}'", url_);
                }
                av_packet_unref(packet_);
                return true;
            }
        }
        av_packet_unref(packet_);
    }
    return false;
}

std::string NativeStreamReader::getCodecName() const {
    if (!codec_ctx_ || !codec_ctx_->codec) return "unknown";
    return codec_ctx_->codec->name;
}

bool NativeStreamReader::getRawPacket(std::vector<unsigned char>& out_data, bool& is_keyframe, uint64_t& pts) {
    return false;
}

} // namespace core
} // namespace vms
