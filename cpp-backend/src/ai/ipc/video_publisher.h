// ipc/video_publisher.h
#pragma once

#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace ipc {

/**
 * Video Publisher using ZeroMQ PUB-SUB pattern
 * 
 * Publishes compressed video frames to Python backend or other subscribers.
 * Uses JPEG compression by default for efficient transmission.
 * 
 * Message format (multipart):
 *   Part 1: Header (24 bytes)
 *     - camera_id: uint32_t (4 bytes)
 *     - frame_id: uint32_t (4 bytes)
 *     - timestamp_ms: uint64_t (8 bytes)
 *     - width: uint32_t (4 bytes)
 *     - height: uint32_t (4 bytes)
 *   Part 2: JPEG or raw BGR data
 */
class VideoPublisher {
public:
    struct Heartbeat {
        uint32_t camera_id;
        uint64_t ts_ms;
        uint64_t frame_id;
        float fps;
    };

    /**
     * Constructor
     * 
     * @param camera_id Camera ID for this publisher
     * @param endpoint ZeroMQ endpoint (default: "tcp://*:555X" where X = camera_id)
     * @param jpeg_quality JPEG compression quality (1-100, default: 85)
     */
    VideoPublisher(int camera_id, const std::string& endpoint = "", int jpeg_quality = 85);
    
    ~VideoPublisher();

    /**
     * Publish a frame (will be JPEG compressed)
     * Topic: frame.<camera_id>
     */
    bool publish_frame(const cv::Mat& frame, uint64_t frame_id, uint64_t timestamp_ms);

    /**
     * Publish heartbeat
     * Topic: hb.<camera_id>
     */
    void send_heartbeat(uint64_t frame_id, float fps);

    bool is_connected() const { return connected_; }

    struct Stats {
        uint64_t frames_sent;
        uint64_t bytes_sent;
        uint64_t dropped_frames; // NEW
        uint64_t errors;
    };

    Stats get_stats() const { return stats_; }
    void reset_stats();

private:
    int camera_id_;
    std::string endpoint_;
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> publisher_;
    bool connected_;
    
    Stats stats_;
    std::vector<int> jpeg_params_;
    float current_fps_ = 0.0f;

    bool connect();
    uint64_t get_timestamp_ms(); // Helper
};

} // namespace ipc

