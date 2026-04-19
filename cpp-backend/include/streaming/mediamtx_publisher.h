#ifndef VMS_STREAMING_MEDIAMTX_PUBLISHER_H
#define VMS_STREAMING_MEDIAMTX_PUBLISHER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QByteArray>
#include <QString>
#include <memory>
#include <chrono>
#include <cstdint>
#include <atomic>
#include "streaming/stream_pipeline.h"
#include "core/brands/camera_adapter_types.hpp"

namespace vms::streaming {

/**
 * @brief Publishes raw H.264 Annex-B packets to a MediaMTX RTSP endpoint.
 *
 * Case B Implementation: Inherits from both QObject and StreamPipeline.
 * Uses a child FFmpeg process: pipe:0 -> rtsp://host:port/cam_<id>
 * Achieves sub-500ms latency via WebRTC WHEP.
 */
class MediaMtxPublisher : public QObject, public StreamPipeline {
    Q_OBJECT
public:
    /**
     * @brief Constructor
     * @param camera_id Camera ID
     * @param profile Stream profile
     * @param config Camera configuration (contains mediamtx_url and ffmpeg_path)
     * @param parent Qt parent object
     */
    MediaMtxPublisher(int camera_id, 
                      const StreamProfile& profile, 
                      const CameraConfig& config, 
                      QObject* parent = nullptr);

    virtual ~MediaMtxPublisher();

    /**
     * @brief Start the FFmpeg publisher process.
     * @return true if started successfully.
     */
    bool start() override;

    /**
     * @brief Stop the publisher process gracefully.
     */
    void stop() override;

    /**
     * @brief Force kill the publisher process.
     */
    void kill() override;

    /**
     * @brief Get the full RTSP publish URL.
     */
    QString getPublishUrl() const { return m_publishUrl; }

public Q_SLOTS:
    /**
     * @brief Slot to receive raw H.264 packets from NativeReaderWorker.
     * Thread-safe via Qt::QueuedConnection.
     * @param data Raw Annex-B packet data
     * @param isKeyframe True if packet is an I-Frame
     */
    void onPacketReady(const QByteArray& data, bool isKeyframe);

private Q_SLOTS:
    /**
     * @brief Internal handler for FFmpeg process termination.
     */
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

    /**
     * @brief Internal handler for stable operation reset.
     */
    void onStableTimer();

    /**
     * @brief Internal handler to read FFmpeg stderr.
     */
    void onReadyReadStandardError();

Q_SIGNALS:
    /**
     * @brief Emitted when the publisher fails after max retries.
     */
    void error(const QString& reason);

protected:
    /**
     * @brief StreamPipeline required method (not used directly for QProcess version).
     */
    std::string buildFFmpegCommand() override;

private:
    void setupStream(const CameraConfig& config);
    void restartProcess();
    void recordDroppedFrame();

    std::unique_ptr<QProcess> m_process;
    QString m_ffmpegExe;
    QString m_publishUrl;
    
    // Watchdog and stability
    QTimer* m_restartTimer = nullptr;
    QTimer* m_stableTimer = nullptr;
    int m_restartAttempts = 0;
    static constexpr int MAX_RESTARTS = 5;
    static constexpr int RESTART_DELAY_MS = 2000;
    static constexpr int STABLE_THRESHOLD_MS = 30000;

    // Keyframe gating — skip all frames until first IDR so FFmpeg can parse SPS/PPS
    bool m_waitingForKeyframe = true;

    // Congestion control with backpressure hysteresis
    bool m_backpressureActive = false;
    bool m_testBackpressureEnabled = false;
    std::chrono::steady_clock::time_point m_lastBackpressureLogAt_{};
    // Stall detection: if pipe doesn't drain below LOW for this long, kill & restart FFmpeg.
    std::chrono::steady_clock::time_point m_lastDrainTime_{};
    static constexpr int STALL_TIMEOUT_SEC = 10;
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> frames_sent_{0};
    // 1 MB high / 128 KB low: tighter hysteresis detects stalls sooner without
    // over-dropping on normal transient bursts.
    static constexpr qint64 BACKPRESSURE_HIGH = 1 * 1024 * 1024;  // 1 MB — stop accepting
    static constexpr qint64 BACKPRESSURE_LOW  = 128 * 1024;       // 128 KB — resume accepting
    static constexpr unsigned long TEST_BACKPRESSURE_DELAY_MS = 50;
};

} // namespace vms::streaming

#endif // VMS_STREAMING_MEDIAMTX_PUBLISHER_H
