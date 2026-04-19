#include "core/native_stream_reader.h"
#include "utils/logger.h"
#include <chrono>
#include <algorithm>
#include <vector>

namespace vms {
namespace core {

// Interrupt callback: aborts av_read_frame if blocked > 2 seconds.
// Called periodically by FFmpeg during blocking I/O.
int NativeStreamReader::interruptCallback(void* opaque) {
    auto* self = static_cast<NativeStreamReader*>(opaque);
    if (!self) return 1;

    // Explicit abort (close() was called)
    if (self->abort_requested_.load()) return 1;

    auto elapsed = std::chrono::steady_clock::now() - self->last_read_start_;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 10000) {
        LOG_WARN("NativeReader: Interrupt triggered - blocked for > 10000ms");
        return 1; // abort — blocked too long
    }
    return 0; // continue
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

    // 1. Open Input
    AVDictionary* options = nullptr;
    const AVInputFormat* input_format = nullptr;
    std::string actual_url = url;

    // ── Webcam support: webcam://0, webcam://1, webcam://USB Camera ──
    if (url.rfind("webcam://", 0) == 0) {
        std::string device_id = url.substr(9); // after "webcam://"
#ifdef _WIN32
        input_format = av_find_input_format("dshow");
        if (!input_format) {
            LOG_ERROR("NativeReader: DirectShow (dshow) not available in this FFmpeg build");
            return false;
        }
        // If numeric index (0, 1, ...), use generic device name
        // User can also pass exact device name like "webcam://USB Camera"
        if (device_id.empty()) device_id = "0";
        
        // For dshow, format is "video=DeviceName"
        // Try numeric → map to "Integrated Camera" style, or use name directly
        bool is_numeric = !device_id.empty() && std::all_of(device_id.begin(), device_id.end(), ::isdigit);
        if (is_numeric) {
            // Use @device_pnp_ prefix for index-based selection on newer FFmpeg,
            // or fallback to common names. Simplest: let dshow enumerate.
            // For index 0, try generic names that work on most systems.
            int idx = std::stoi(device_id);
            if (idx == 0) {
                actual_url = "video=Integrated Webcam";
            } else {
                actual_url = "video=USB Camera " + device_id;
            }
            LOG_INFO("NativeReader: Webcam index {} -> trying '{}'", idx, actual_url);
        } else {
            actual_url = "video=" + device_id;
        }
        
        // Webcam-specific options
        av_dict_set(&options, "video_size", "1280x720", 0);
        av_dict_set(&options, "framerate", "30", 0);
        av_dict_set(&options, "pixel_format", "yuyv422", 0); // Common webcam format
#else
        input_format = av_find_input_format("v4l2");
        if (!input_format) {
            LOG_ERROR("NativeReader: V4L2 not available in this FFmpeg build");
            return false;
        }
        actual_url = "/dev/video" + device_id;
        av_dict_set(&options, "video_size", "1280x720", 0);
        av_dict_set(&options, "framerate", "30", 0);
#endif
        LOG_INFO("NativeReader: Opening webcam device: '{}'", actual_url);
    } else {
        // ── RTSP / Network stream ──
        av_dict_set(&options, "rtsp_transport", "tcp", 0);          // force TCP — avoids UDP packet loss
        av_dict_set(&options, "stimeout", "5000000", 0);             // 5s socket timeout (µs)
        av_dict_set(&options, "probesize", "500000", 0);             // 500 KB — reduce initial latency
        av_dict_set(&options, "analyzeduration", "1000000", 0);      // 1s — reduce analyze delay
        av_dict_set(&options, "fflags", "nobuffer+discardcorrupt", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        // Remove reorder_queue_size to prevent B-frame latency buffering in libavformat
        av_dict_set(&options, "buffer_size", "1048576", 0);          // 1 MB — enough for local subnet
        av_dict_set(&options, "max_delay", "0", 0);
        // NOTE: libav reconnect flags intentionally omitted. They silently retry internally
        // and hide actual disconnection events from the worker's backoff/state-machine logic.
        // All reconnect policy is owned by NativeReaderWorker (exponential backoff + state transitions).
    }

    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        LOG_ERROR("NativeReader: Failed to allocate format context");
        av_dict_free(&options);
        return false;
    }

    // Install interrupt callback BEFORE open — prevents hanging on unresponsive cameras
    last_read_start_ = std::chrono::steady_clock::now();
    fmt_ctx_->interrupt_callback.callback = &NativeStreamReader::interruptCallback;
    fmt_ctx_->interrupt_callback.opaque = this;

    int ret = avformat_open_input(&fmt_ctx_, actual_url.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        
        // For webcam: if default name fails, try with index-based enumeration
        if (url.rfind("webcam://", 0) == 0) {
            LOG_WARN("NativeReader: Failed with '{}': {} — trying alternative device names...", actual_url, err);
            
            // Try common webcam names on Windows
            std::vector<std::string> fallback_names = {
                "video=Integrated Camera",
                "video=USB Video Device",
                "video=HD Webcam",
                "video=Webcam",
                "video=USB2.0 HD UVC WebCam",
                "video=HP Wide Vision HD Camera",
                "video=Lenovo EasyCamera"
            };
            
            bool opened = false;
            for (const auto& name : fallback_names) {
                if (name == actual_url) continue; // Skip already tried
                
                fmt_ctx_ = avformat_alloc_context();
                fmt_ctx_->interrupt_callback.callback = &NativeStreamReader::interruptCallback;
                fmt_ctx_->interrupt_callback.opaque = this;
                last_read_start_ = std::chrono::steady_clock::now();
                
                AVDictionary* retry_opts = nullptr;
                av_dict_set(&retry_opts, "video_size", "1280x720", 0);
                av_dict_set(&retry_opts, "framerate", "30", 0);
                
                LOG_INFO("NativeReader: Trying webcam name: '{}'", name);
                ret = avformat_open_input(&fmt_ctx_, name.c_str(), input_format, &retry_opts);
                av_dict_free(&retry_opts);
                
                if (ret == 0) {
                    actual_url = name;
                    opened = true;
                    LOG_INFO("NativeReader: ✅ Webcam opened with name: '{}'", name);
                    break;
                }
                // Clean up failed context
                if (fmt_ctx_) {
                    avformat_close_input(&fmt_ctx_);
                    fmt_ctx_ = nullptr;
                }
            }
            
            if (!opened) {
                LOG_ERROR("NativeReader: Could not open any webcam device. "
                          "Use 'ffmpeg -list_devices true -f dshow -i dummy' to list available devices, "
                          "then use 'webcam://Exact Device Name' as the RTSP URL.");
                return false;
            }
        } else {
            LOG_ERROR("NativeReader: Failed to open input '{}': {}", url, err);
            return false;
        }
    }

    // 2. Find Stream Info
    last_read_start_ = std::chrono::steady_clock::now();
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        LOG_ERROR("NativeReader: Failed to find stream info for '{}'", url);
        cleanup();
        return false;
    }

    // 3. Find Video Stream
    video_stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        LOG_ERROR("NativeReader: No video stream found in '{}'", url);
        cleanup();
        return false;
    }

    AVStream* stream = fmt_ctx_->streams[video_stream_index_];

    // 4. Find Decoder
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        LOG_ERROR("NativeReader: No decoder found for codec ID {}", static_cast<int>(stream->codecpar->codec_id));
        cleanup();
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(decoder);
    if (!codec_ctx_) {
        LOG_ERROR("NativeReader: Failed to allocate codec context");
        cleanup();
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0) {
        LOG_ERROR("NativeReader: Failed to copy params to codec context");
        cleanup();
        return false;
    }

    // Multi-threading for the decoder
    codec_ctx_->thread_count = 0; // 0 = automatic
    codec_ctx_->thread_type = FF_THREAD_FRAME;

    // Suppress "reference picture missing" warnings on mid-GOP connect
    codec_ctx_->err_recognition = 0;

    if (avcodec_open2(codec_ctx_, decoder, nullptr) < 0) {
        LOG_ERROR("NativeReader: Failed to open decoder");
        cleanup();
        return false;
    }

    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;
    if (stream->avg_frame_rate.den != 0) {
        fps_ = av_q2d(stream->avg_frame_rate);
    }
    // FIX: Some cameras report 0 FPS (e.g. stream info incomplete).
    // Use 25fps fallback so the pipeline doesn't stall or report "0 FPS".
    if (fps_ <= 0.0) {
        fps_ = 25.0;
        LOG_WARN("NativeReader: FPS not detected for '{}', using fallback {}fps", url, fps_);
    }

    // 5. Prepare Frames & Packet
    frame_ = av_frame_alloc();
    frame_bgr_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    if (!frame_ || !frame_bgr_ || !packet_) {
        LOG_ERROR("NativeReader: Failed to allocate frames/packet");
        cleanup();
        return false;
    }

    connected_ = true;
    LOG_INFO("NativeReader: Connected to '{}' ({}x{} @ {} FPS)", url, width_, height_, fps_);
    return true;
}

void NativeStreamReader::abort() {
    // Thread-safe: only sets atomic flags.
    // The interrupt callback will see abort_requested_ and cause av_read_frame to return.
    // readFrame()'s while-loop will see connected_==false and exit.
    abort_requested_ = true;
    connected_ = false;
}

void NativeStreamReader::close() {
    abort_requested_ = true;
    connected_ = false;
    cleanup();
}

void NativeStreamReader::cleanup() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (frame_bgr_) {
        av_frame_free(&frame_bgr_);
        frame_bgr_ = nullptr;
    }
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
}

bool NativeStreamReader::readFrame(cv::Mat& out_mat) {
    if (!connected_) return false;

    // Reset interrupt timer before each read attempt
    last_read_start_ = std::chrono::steady_clock::now();

    int ret;
    while (connected_ && !abort_requested_.load()) {
        ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            if (ret == AVERROR_EXIT || ret == AVERROR(ETIMEDOUT)) {
                int timeouts = ++consecutive_timeouts_;
                LOG_WARN("[NETWORK] NativeReader: TCP read timeout ({}/3) for '{}' — camera may be unreachable",
                         timeouts, url_);
                if (timeouts >= 3) {
                    LOG_ERROR("[NETWORK] NativeReader: 3 consecutive TCP timeouts for '{}' — forcing reconnect",
                              url_);
                    connected_ = false;
                }
            } else if (ret == AVERROR(ECONNRESET) || ret == AVERROR(ECONNREFUSED) ||
                       ret == AVERROR(ENETUNREACH) || ret == AVERROR(EHOSTUNREACH)) {
                LOG_ERROR("[NETWORK] NativeReader: Connection lost for '{}': {} — will reconnect", url_, err_buf);
                connected_ = false;
            } else if (ret == AVERROR_EOF) {
                LOG_WARN("[NETWORK] NativeReader: Stream EOF for '{}' — camera closed connection", url_);
                connected_ = false;
            } else {
                LOG_ERROR("[DECODE] NativeReader: av_read_frame error for '{}': {}", url_, err_buf);
                connected_ = false;
            }
            break;
        }

        // Reset timer and counter after successful packet read
        last_read_start_ = std::chrono::steady_clock::now();
        consecutive_timeouts_ = 0;

        if (packet_->stream_index == video_stream_index_) {
            if (packet_callback_) {
                bool is_key = (packet_->flags & AV_PKT_FLAG_KEY);
                // FIX: Prepend SPS/PPS (extradata) to every keyframe so that
                // downstream consumers (MediaMTX FFmpeg) can decode from any IDR.
                if (is_key && codec_ctx_ && codec_ctx_->extradata && codec_ctx_->extradata_size > 0) {
                    std::vector<uint8_t> full_packet;
                    full_packet.reserve(codec_ctx_->extradata_size + packet_->size);
                    full_packet.insert(full_packet.end(), 
                                       codec_ctx_->extradata, 
                                       codec_ctx_->extradata + codec_ctx_->extradata_size);
                    full_packet.insert(full_packet.end(), 
                                       packet_->data, 
                                       packet_->data + packet_->size);
                    packet_callback_(full_packet.data(), (int)full_packet.size(), true);
                } else {
                    packet_callback_(packet_->data, packet_->size, is_key);
                }
            }

            ret = avcodec_send_packet(codec_ctx_, packet_);
            if (ret < 0) {
                av_packet_unref(packet_);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx_, frame_);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    av_packet_unref(packet_);
                    return false;
                }

                if (frame_->decode_error_flags != 0) {
                    LOG_WARN("[DECODE] NativeReader: Corrupted frame skipped (error_flags=0x{:x}) for '{}' "
                             "— likely missing reference frames on mid-GOP connect",
                             frame_->decode_error_flags, url_);
                    continue;
                }

                // Convert to BGR for OpenCV
                if (!sws_ctx_) {
                    sws_ctx_ = sws_getContext(
                        codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
                        codec_ctx_->width, codec_ctx_->height, AV_PIX_FMT_BGR24,
                        SWS_BILINEAR, nullptr, nullptr, nullptr
                    );
                }

                if (out_mat.empty() || out_mat.rows != codec_ctx_->height || out_mat.cols != codec_ctx_->width) {
                    out_mat = cv::Mat(codec_ctx_->height, codec_ctx_->width, CV_8UC3);
                }

                uint8_t* dest[4] = { out_mat.data, nullptr, nullptr, nullptr };
                int dest_linesize[4] = { static_cast<int>(out_mat.step), 0, 0, 0 };

                sws_scale(
                    sws_ctx_, frame_->data, frame_->linesize, 0, codec_ctx_->height,
                    dest, dest_linesize
                );

                if (!first_frame_decoded_) {
                    first_frame_decoded_ = true;
                    LOG_INFO("NativeReader: First frame decoded for camera '{}'", url_);
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
