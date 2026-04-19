#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <ctime>
#include <chrono>

namespace vms {

// Forward declare
namespace core { class FFmpegProcess; }

namespace recording {

/**
 * @brief Continuous 24/7 recorder for a single camera
 * 
 * Uses FFmpeg segment muxer to split RTSP stream into 5-minute MP4 segments.
 * Each completed segment is registered in the recording_segments DB table.
 * Old segments are automatically pruned based on retention policy (default: 7 days).
 * 
 * Thread-safety: Not thread-safe. Each instance runs its own internal thread
 * but must not be accessed concurrently from external threads without synchronization.
 */
class ContinuousRecorder {
public:
    /**
     * @param camera_id   DB camera ID
     * @param rtsp_url    RTSP stream URL
     * @param output_dir  Base directory for segment files (default: "recordings")
     * @param segment_sec Segment duration in seconds (default: 300 = 5 min)
     * @param retention_days Number of days to keep old segments (default: 7)
     */
    ContinuousRecorder(int camera_id, const std::string& rtsp_url,
                       const std::string& output_dir = "recordings",
                       int segment_sec = 300,
                       int retention_days = 7);
    ~ContinuousRecorder();

    // Non-copyable
    ContinuousRecorder(const ContinuousRecorder&) = delete;
    ContinuousRecorder& operator=(const ContinuousRecorder&) = delete;

    bool start();
    void stop();
    void kill();
    bool isRunning() const { return running_.load(); }
    int getCameraId() const { return camera_id_; }

    // Feed raw H264 data from NativeReader into the muxer
    void writeRawData(const uint8_t* data, int size);

private:
    void recorderLoop();
    void pruneOldSegments();
    void scanAndRegisterSegments();
    std::string buildSegmentDir() const;

    const int camera_id_;
    std::string rtsp_url_;
    std::string output_dir_;
    const int segment_sec_;
    const int retention_days_;

    std::atomic<bool> should_stop_{false};
    std::atomic<bool> running_{false};
    std::thread recorder_thread_;
    std::unique_ptr<core::FFmpegProcess> ffmpeg_;
    
    // Per-instance prune timer (previously was a buggy static local)
    std::chrono::steady_clock::time_point last_prune_time_{std::chrono::steady_clock::now()};
};

} // namespace recording
} // namespace vms
