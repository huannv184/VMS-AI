// ipc/video_publisher.cpp
// Thread-safety: Not thread-safe. Each VideoPublisher instance is owned
// by a single AI worker thread.
#include "video_publisher.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace ipc {

VideoPublisher::VideoPublisher(int camera_id, const std::string& endpoint, int jpeg_quality)
    : camera_id_(camera_id)
    , endpoint_(endpoint)
    , connected_(false)
{
    // Use tcp://127.0.0.1:PORT for explicit local loopback (safer on Windows)
    if (endpoint_.empty()) {
        endpoint_ = "tcp://127.0.0.1:" + std::to_string(5555 + camera_id_);
    }

    jpeg_params_ = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality};

    spdlog::info("[VideoPublisher] Initializing for Camera {} on {} (Quality: {})",
                 camera_id_, endpoint_, jpeg_quality);

    reset_stats();
    connect();
}

VideoPublisher::~VideoPublisher() {
    if (publisher_) {
        try {
            publisher_->close();
        } catch (...) {
            // ZMQ close() may throw during shutdown — safe to ignore
        }
    }
    spdlog::info("[VideoPublisher] Shutdown Stats: Sent={}, Bytes={}MB, Dropped={}",
                 stats_.frames_sent, stats_.bytes_sent / 1024 / 1024, stats_.dropped_frames);
}

bool VideoPublisher::connect() {
    try {
        context_ = std::make_unique<zmq::context_t>(1);
        publisher_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::pub);

        // PRODUCTION TUNING
        int hwm = 2; // Very small buffer (drop old frames quickly)
        publisher_->set(zmq::sockopt::sndhwm, hwm);

        int timeo = 0; // Non-blocking send
        publisher_->set(zmq::sockopt::sndtimeo, timeo);

        int linger = 0; // Don't wait on close
        publisher_->set(zmq::sockopt::linger, linger);

        publisher_->bind(endpoint_);
        
        connected_ = true;
        spdlog::info("[VideoPublisher] Bound to {}", endpoint_);
        return true;

    } catch (const zmq::error_t& e) {
        spdlog::error("[VideoPublisher] Bind failed: {}", e.what());
        connected_ = false;
        return false;
    }
}

uint64_t VideoPublisher::get_timestamp_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool VideoPublisher::publish_frame(const cv::Mat& frame, uint64_t frame_id, uint64_t timestamp_ms) {
    if (!connected_ || frame.empty()) return false;

    // Encode JPEG
    std::vector<uchar> jpeg_buffer;
    if (!cv::imencode(".jpg", frame, jpeg_buffer, jpeg_params_)) {
        stats_.errors++;
        return false;
    }

    // Topic: frame.<camera_id>
    std::string topic = "frame." + std::to_string(camera_id_);

    try {
        // Prepare messages
        zmq::message_t topic_msg(topic.data(), topic.size());
        zmq::message_t data_msg(jpeg_buffer.data(), jpeg_buffer.size());

        // ATOMIC SEND (SNDMORE + DONTWAIT)
        // If first part fails (EAGAIN), it returns false immediately.
        // If first part succeeds but second fails, ZMQ handles drop or queueing?
        // With PUB socket, DONTWAIT generally drops if queue full.
        
        bool ok = publisher_->send(topic_msg, zmq::send_flags::sndmore | zmq::send_flags::dontwait).has_value();
        if (ok) {
            bool part2_ok = publisher_->send(data_msg, zmq::send_flags::dontwait).has_value();
            if (!part2_ok) {
                // FAILURE CASE: First part sent, second failed. 
                // Socket is now in "expecting more" state (or buffer has partial message).
                // We MUST reset the state or flush the multipart sequence.
                // Sending an empty message without SNDMORE terminates the sequence.
                publisher_->send(zmq::message_t(), zmq::send_flags::dontwait);
                ok = false;
            }
        }

        if (!ok) {
            stats_.dropped_frames++;
            return false;
        }

        stats_.frames_sent++;
        stats_.bytes_sent += (topic.size() + jpeg_buffer.size());
        return true;

    } catch (...) {
        stats_.errors++;
        return false;
    }
}

void VideoPublisher::send_heartbeat(uint64_t frame_id, float fps) {
    if (!connected_) return;

    Heartbeat hb;
    hb.camera_id = camera_id_;
    hb.ts_ms = get_timestamp_ms();
    hb.frame_id = frame_id;
    hb.fps = fps;

    std::string topic = "hb." + std::to_string(camera_id_);

    try {
        // DEBUG: Print heartbeat
        spdlog::debug("[VideoPublisher] Sending HB..."); 
        
        zmq::message_t topic_msg(topic.data(), topic.size());
        zmq::message_t data_msg(&hb, sizeof(hb));

        publisher_->send(topic_msg, zmq::send_flags::sndmore | zmq::send_flags::dontwait);
        publisher_->send(data_msg, zmq::send_flags::dontwait);
    } catch (...) {
        // Ignore heartbeat errors
    }
}

void VideoPublisher::reset_stats() {
    stats_ = {0, 0, 0, 0};
}

} // namespace ipc