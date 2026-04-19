#pragma once

#include <QThread>
#include <QByteArray>
#include <QMutex>
#include <QFuture>
#include <QThreadPool>
#include <atomic>
#include <opencv2/opencv.hpp>
#include "core/native_stream_reader.h"
#include "inference/tracking.h"
#include <nlohmann/json.hpp>
#include <QMetaType>

// Forward declaration to avoid heavy AI headers in common worker
namespace ipc { class SharedMemoryManager; }

// Declare cv::Mat and custom types as Qt meta types to allow cross-thread signal/slot passing
Q_DECLARE_METATYPE(cv::Mat)
Q_DECLARE_METATYPE(std::vector<uchar>)
Q_DECLARE_METATYPE(std::vector<inference::TrackedObject>)
Q_DECLARE_METATYPE(nlohmann::json)

namespace vms {
namespace core {

class NativeReaderWorker : public QThread {
    Q_OBJECT
public:
    explicit NativeReaderWorker(int camera_id, const std::string& url, QObject* parent = nullptr);
    ~NativeReaderWorker() override;

    void setShmManager(::ipc::SharedMemoryManager* shm) { shm_manager_ = shm; }

    // Prevent copying
    NativeReaderWorker(const NativeReaderWorker&) = delete;
    NativeReaderWorker& operator=(const NativeReaderWorker&) = delete;

    void stop();

    // Adaptive FPS: safe to call from any thread.
    // The run() loop reads this on each frame to compute its emit interval.
    void setTargetFps(int fps) { target_fps_.store(fps, std::memory_order_relaxed); }
    int  getTargetFps() const  { return target_fps_.load(std::memory_order_relaxed); }

Q_SIGNALS:
    // Emitted when a new frame is decoded
    void frameReady(const cv::Mat& frame);
    // Emitted when a frame is processed (resized, encoded, AI metadata attached)
    // This allows the main thread to remain lean.
    void frameProcessed(int camera_id, 
                        const std::vector<uchar>& jpeg_data, 
                        const std::vector<inference::TrackedObject>& objects,
                        const nlohmann::json& meta,
                        uint64_t timestamp);
    // Emitted when raw packets are received (for zero-copy recording)
    void rawPacketReady(const QByteArray& data, bool isKeyframe);
    // Emitted when connection is established
    void streamConnected();
    // Emitted when connection is lost
    void streamDisconnected();
    // Emitted when all reconnect attempts are exhausted — camera is permanently FAILED
    void streamFailed();

protected:
    void run() override;

private:
    int camera_id_;
    std::string url_;
    std::atomic<bool> should_stop_{false};
    // Target processing FPS (not decode FPS — stream is always fully read).
    // 5 fps idle (no live viewers), 15 fps active.  Default 15 until first watchdog tick.
    std::atomic<int> target_fps_{15};
    std::unique_ptr<NativeStreamReader> reader_;
    ::ipc::SharedMemoryManager* shm_manager_{nullptr};
    uint64_t frame_count_{0};
    QFuture<void> processing_future_;
    // Per-camera pool (1 thread): isolates processing from other cameras'
    // tasks in the global pool, preventing cross-camera starvation.
    QThreadPool processing_pool_;
};

} // namespace core
} // namespace vms
