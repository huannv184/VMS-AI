#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}
#include <functional>

namespace vms {
namespace core {

/**
 * NativeStreamReader - Uses LibAV (FFmpeg C API) for low-latency RTSP acquisition.
 * Replaces FFmpegProcess subprocess logic.
 */
class NativeStreamReader {
public:
    NativeStreamReader();
    ~NativeStreamReader();

    // Prevent copying
    NativeStreamReader(const NativeStreamReader&) = delete;
    NativeStreamReader& operator=(const NativeStreamReader&) = delete;

    /**
     * Open RTSP stream.
     */
    bool open(const std::string& url);

    /**
     * Close stream and release resources.
     * MUST only be called after the reader thread has fully stopped.
     */
    void close();

    /**
     * Signal the reader to abort without freeing resources.
     * Thread-safe: can be called from any thread while readFrame() is active.
     * The interrupt callback will cause av_read_frame to return immediately.
     */
    void abort();

    /**
     * Read and decode the next frame.
     * Returns true on success, false on error or EOF.
     * @param out_mat Output OpenCV Matrix (BGR)
     */
    bool readFrame(cv::Mat& out_mat);

    /**
     * Get the latest raw packet (NALU) for streaming without re-encoding.
     */
    bool getRawPacket(std::vector<unsigned char>& out_data, bool& is_keyframe, uint64_t& pts);

    /**
     * Check if stream is connected.
     */
    bool isConnected() const { return connected_.load(); }

    /**
     * Set callback to receive raw packets directly from the demuxer (for zero-copy recording)
     */
    using PacketCallback = std::function<void(const uint8_t* data, int size, bool is_keyframe)>;
    void setPacketCallback(PacketCallback cb) { packet_callback_ = cb; }

    // Stream info
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    double getFps() const { return fps_; }
    std::string getCodecName() const;

private:
    std::atomic<bool> connected_{false};
    std::string url_;

    // FFmpeg structures
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;

    int video_stream_index_ = -1;
    AVFrame* frame_ = nullptr;
    AVFrame* frame_bgr_ = nullptr;
    AVPacket* packet_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    bool first_frame_decoded_{false};
    std::atomic<int> consecutive_timeouts_{0};

    // Interrupt callback state — aborts av_read_frame if blocked > 2s
    std::atomic<bool> abort_requested_{false};
    std::chrono::steady_clock::time_point last_read_start_;
    static int interruptCallback(void* opaque);

    void cleanup();

    PacketCallback packet_callback_;
};

} // namespace core
} // namespace vms
