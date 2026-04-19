#include "streaming/mediamtx_publisher.h"
#include <algorithm>
#include "core/ffmpeg_process.h"
#include "core/process_manager.h"
#include "core/runtime_state.h"
#include "utils/config.h"
#include "utils/logger.h"
#include <QDebug>
#include <QThread>
#include <chrono>

namespace vms::streaming {

MediaMtxPublisher::MediaMtxPublisher(int camera_id, 
                                     const StreamProfile& profile, 
                                     const CameraConfig& config, 
                                     QObject* parent)
    : QObject(parent)
    , StreamPipeline(camera_id, profile) {
    
    setupStream(config);

    // Watchdog delay timer
    m_restartTimer = new QTimer(this);
    m_restartTimer->setSingleShot(true);
    connect(m_restartTimer, &QTimer::timeout, this, &MediaMtxPublisher::restartProcess);

    // Stability timer
    m_stableTimer = new QTimer(this);
    m_stableTimer->setSingleShot(true);
    connect(m_stableTimer, &QTimer::timeout, this, &MediaMtxPublisher::onStableTimer);
}

MediaMtxPublisher::~MediaMtxPublisher() {
    stop();
}

void MediaMtxPublisher::setupStream(const CameraConfig& config) {
    m_testBackpressureEnabled = (qgetenv("VMS_TEST_BACKPRESSURE") == "1") ||
                                (vms::Config::getInstance().getServerConfig().port == 18300);
    if (m_testBackpressureEnabled) {
        LOG_WARN("[MediaMTX-{}] Backpressure test mode enabled (delay={}ms)",
                 camera_id_, TEST_BACKPRESSURE_DELAY_MS);
    }

    // R5: Resolve FFmpeg executable
    if (!config.ffmpeg_path.empty()) {
        m_ffmpegExe = QString::fromStdString(config.ffmpeg_path);
    } else {
        // Use the shared getFfmpegPath() resolver instead of bare "ffmpeg"
        m_ffmpegExe = QString::fromStdString(vms::core::getFfmpegPath());
    }

    // R6: Resolve MediaMTX URL
    QString base;
    if (config.mediamtx_url.empty()) {
        base = QStringLiteral("rtsp://localhost:8554");
        // Log notice once per camera startup
        qInfo() << "[MediaMTX][" << camera_id_ << "] mediamtx_url empty, falling back to" << base;
    } else {
        base = QString::fromStdString(config.mediamtx_url);
    }

    // Canonical publish URL: rtsp://{base}/cam_{id}
    // Remove trailing slash if present in base to avoid //
    if (base.endsWith('/')) base.chop(1);
    m_publishUrl = QString("%1/cam_%2").arg(base).arg(camera_id_);
}

bool MediaMtxPublisher::start() {
    if (vms::core::shutting_down.load(std::memory_order_acquire)) {
        return false;
    }

    if (m_process && m_process->state() != QProcess::NotRunning) return true;

    m_process = std::make_unique<QProcess>(this);

    // Reset keyframe gate so we wait for the first IDR before piping
    m_waitingForKeyframe = true;

    // R5: Extract Argument List (Exact flags and order)
    // -analyzeduration 0 -probesize 32: don't wait for extra data — SPS/PPS in the
    // first keyframe is sufficient because we gate on IDR before writing to the pipe.
    QStringList args;
    args << "-loglevel" << "warning"
         << "-analyzeduration" << "0"
         << "-probesize" << "32"
         << "-f" << "h264"
         << "-i" << "pipe:0"
         << "-c" << "copy"
         << "-f" << "rtsp"
         << m_publishUrl;

    connect(m_process.get(), &QProcess::readyReadStandardError, this, &MediaMtxPublisher::onReadyReadStandardError);
    connect(m_process.get(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MediaMtxPublisher::onProcessFinished);

    LOG_INFO("[MediaMTX-{}] Starting publisher: {} {}", 
             camera_id_, m_ffmpegExe.toStdString(), args.join(" ").toStdString());

    m_process->start(m_ffmpegExe, args);

    if (!m_process->waitForStarted(2000)) {
        LOG_ERROR("[MediaMTX-{}] Failed to start FFmpeg process: {}", camera_id_, m_process->errorString().toStdString());
        return false;
    }

    vms::core::ProcessManager::getInstance().registerProcess(
        m_process.get(), "mediamtx-ffmpeg-camera-" + std::to_string(camera_id_));

    // Reset stall tracking so we don't carry stale state into the new process.
    m_lastDrainTime_ = std::chrono::steady_clock::now();
    m_backpressureActive = false;

    setState(vms::core::PipelineState::RUNNING);
    m_stableTimer->start(STABLE_THRESHOLD_MS);
    return true;
}

void MediaMtxPublisher::stop() {
    if (!m_process) return;

    // Timers and QProcess must be stopped on the owner thread.
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, &MediaMtxPublisher::stop, Qt::BlockingQueuedConnection);
        return;
    }

    LOG_INFO("[MediaMTX-{}] Stopping publisher...", camera_id_);
    setState(vms::core::PipelineState::STOPPING);

    // Stop timers to prevent watchdog from fighting the stop()
    m_restartTimer->stop();
    m_stableTimer->stop();

    // R7: Graceful shutdown sequence
    // First, disconnect the finished signal so we don't trigger watchdog auto-restart
    disconnect(m_process.get(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MediaMtxPublisher::onProcessFinished);

    m_process->terminate();
    if (!m_process->waitForFinished(2000)) {
        LOG_WARN("[MediaMTX-{}] Process did not terminate, killing...", camera_id_);
        m_process->kill();
        m_process->waitForFinished(500);
    }

    vms::core::ProcessManager::getInstance().unregisterProcess(m_process.get());
    m_process.reset();
    setState(vms::core::PipelineState::STOPPED);
}

void MediaMtxPublisher::kill() {
    if (m_process) {
        m_process->kill();
        vms::core::ProcessManager::getInstance().unregisterProcess(m_process.get());
    }
}

void MediaMtxPublisher::onPacketReady(const QByteArray& data, bool isKeyframe) {
    // R2 & R3: Inter-thread write logic
    // Qt::QueuedConnection ensures this runs on the publisher's thread.

    if (!m_process || m_process->state() != QProcess::Running) {
        return;
    }

    // FIX: Keyframe gate — skip all frames before the first IDR.
    // Without SPS/PPS (carried in/before the first keyframe), FFmpeg cannot
    // determine codec parameters and the publish will fail with -22.
    if (m_waitingForKeyframe) {
        if (!isKeyframe) {
            return; // Silently drop pre-IDR frames
        }
        m_waitingForKeyframe = false;
        LOG_INFO("[MediaMTX-{}] First keyframe received — starting pipe to FFmpeg", camera_id_);
    }

    if (m_testBackpressureEnabled) {
        QThread::msleep(TEST_BACKPRESSURE_DELAY_MS);
    }

    // Backpressure with hysteresis.
    // HIGH (1 MB): activate — start dropping P/B frames, accept only keyframes.
    // LOW  (128 KB): deactivate — resume normal write.
    qint64 buffered = m_process->bytesToWrite();
    if (m_testBackpressureEnabled && !isKeyframe) {
        buffered += BACKPRESSURE_HIGH;
    }

    if (m_backpressureActive) {
        // Stall detection: if the pipe has not drained below LOW for STALL_TIMEOUT_SEC,
        // FFmpeg is likely disconnected from MediaMTX or blocked. Kill and let the
        // watchdog restart it with a fresh connection.
        auto stall_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_lastDrainTime_).count();
        if (stall_sec >= STALL_TIMEOUT_SEC) {
            LOG_WARN("[MediaMTX-{}] Pipe stalled for {}s (buf={} KB, dropped={}) — restarting FFmpeg",
                     camera_id_, stall_sec, buffered / 1024,
                     frames_dropped_.load(std::memory_order_relaxed));
            // kill() triggers onProcessFinished → watchdog restart
            if (m_process) m_process->kill();
            return;
        }

        if (buffered < BACKPRESSURE_LOW) {
            m_backpressureActive = false;
            m_lastDrainTime_ = std::chrono::steady_clock::now();
            // Reset drop counter so the next backpressure episode starts fresh.
            frames_dropped_.store(0, std::memory_order_relaxed);
            LOG_INFO("[MediaMTX-{}] Backpressure released (buf={} KB)",
                     camera_id_, buffered / 1024);
        } else {
            // Still under pressure — drop P/B frames, always accept keyframes so
            // decoder can recover on the far end.
            if (!isKeyframe) {
                recordDroppedFrame();
                return;
            }
        }
    } else if (buffered > BACKPRESSURE_HIGH) {
        m_backpressureActive = true;
        // Record the moment pressure started so stall timeout is measured correctly.
        m_lastDrainTime_ = std::chrono::steady_clock::now();
        recordDroppedFrame();
        return;
    }

    m_process->write(data);
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    frames_processed_++;
    bytes_received_ += data.size();
}

void MediaMtxPublisher::recordDroppedFrame() {
    const auto dropped = frames_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto now = std::chrono::steady_clock::now();
    if (m_lastBackpressureLogAt_.time_since_epoch().count() == 0 ||
        now - m_lastBackpressureLogAt_ >= std::chrono::seconds(5)) {
        m_lastBackpressureLogAt_ = now;
        LOG_WARN("[MediaMTX-{}] Pipe backpressure: dropped {} frames this episode",
                 camera_id_, dropped);
    }
}

void MediaMtxPublisher::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (m_process) {
        vms::core::ProcessManager::getInstance().unregisterProcess(m_process.get());
    }

    LOG_WARN("[MediaMTX-{}] Publisher process finished with code {} (Stability Count: {})", 
             camera_id_, exitCode, m_restartAttempts);
    
    m_stableTimer->stop();
    setState(vms::core::PipelineState::FAILING);

    // R4: Watchdog Auto-restart logic with exponential backoff
    // Delay: 2s → 4s → 8s → 16s → 32s. Caps at MAX_RESTARTS to avoid endless retry storms.
    if (!vms::core::shutting_down.load(std::memory_order_acquire) &&
        m_restartAttempts < MAX_RESTARTS) {
        // Exponential backoff: RESTART_DELAY_MS * 2^attempt, capped at 30s
        int delay_ms = static_cast<int>(
            std::min<long long>(RESTART_DELAY_MS * (1LL << m_restartAttempts), 30000LL));
        m_restartAttempts++;
        if (m_restartAttempts >= 3) {
            LOG_WARN("[MediaMTX-{}] Repeated failures — MediaMTX may be unreachable on {}. "
                     "Auto-restarting in {} ms (Attempt {}/{})",
                     camera_id_, m_publishUrl.toStdString(), delay_ms, m_restartAttempts, MAX_RESTARTS);
        } else {
            LOG_INFO("[MediaMTX-{}] Auto-restarting in {} ms (Attempt {}/{})",
                     camera_id_, delay_ms, m_restartAttempts, MAX_RESTARTS);
        }
        m_restartTimer->start(delay_ms);
    } else {
        QString reason = QString("Maximum restart attempts (%1) reached — MediaMTX unreachable at %2")
            .arg(MAX_RESTARTS).arg(m_publishUrl);
        LOG_ERROR("[MediaMTX-{}] {}", camera_id_, reason.toStdString());
        Q_EMIT error(reason);
        setState(vms::core::PipelineState::STOPPED);
    }
}

void MediaMtxPublisher::restartProcess() {
    if (vms::core::shutting_down.load(std::memory_order_acquire)) {
        return;
    }

    // FIX: Reset pipeline state on restart so we don't carry stale counters/flags
    // from the crashed session. The keyframe gate ensures FFmpeg gets a clean
    // SPS/PPS + IDR on the new pipe.
    m_waitingForKeyframe = true;
    m_backpressureActive = false;
    m_lastBackpressureLogAt_ = std::chrono::steady_clock::time_point{};
    m_lastDrainTime_ = std::chrono::steady_clock::now();
    frames_dropped_.store(0, std::memory_order_relaxed);
    frames_sent_.store(0, std::memory_order_relaxed);
    frames_processed_ = 0;
    bytes_received_ = 0;
    start();
}

void MediaMtxPublisher::onStableTimer() {
    if (m_restartAttempts > 0) {
        LOG_INFO("[MediaMTX-{}] Stable threshold reached. Resetting restart counter.", camera_id_);
        m_restartAttempts = 0;
    }
}

void MediaMtxPublisher::onReadyReadStandardError() {
    if (!m_process) return;
    QByteArray data = m_process->readAllStandardError();
    if (!data.isEmpty()) {
        // R5: Forward stderr to qWarning() with prefix
        qWarning() << QString("[MediaMTX][cam_%1] FFmpeg: %2").arg(camera_id_).arg(QString::fromUtf8(data).trimmed());
    }
}

std::string MediaMtxPublisher::buildFFmpegCommand() {
    // Not used by this QProcess-based implementation, but required by StreamPipeline
    return "ffmpeg [managed by QProcess]";
}

} // namespace vms::streaming
