#include "core/media_pipeline.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include "core/brands/camera_adapter_types.hpp"
#include "core/native_reader_worker.h"
#include "recording/buffer_pipeline.h"
#include "recording/continuous_recorder.h"
#include "streaming/mediamtx_publisher.h"
#include "streaming/stream_types.h"
#include "utils/logger.h"

namespace vms::core {

MediaPipeline::MediaPipeline(Config cfg) : cfg_(std::move(cfg)) {}

MediaPipeline::~MediaPipeline() {
    stop();
}

bool MediaPipeline::startBuffer() {
    if (buffer_) return true;
    vms::streaming::StreamProfile rec_profile;
    rec_profile.rtsp_url = cfg_.live_url;
    buffer_ = std::make_unique<vms::recording::BufferPipeline>(cfg_.camera_id, rec_profile);
    if (!buffer_->start()) {
        LOG_WARN("[MediaPipeline] Camera {} BufferPipeline failed to start", cfg_.camera_id);
        // Keep the object — writeRawData() will still no-op safely if internal state
        // is invalid. Returning false lets the caller log/notify if it cares.
        return false;
    }
    return true;
}

bool MediaPipeline::startMediaMtx() {
    if (!cfg_.mediamtx_enabled) {
        LOG_INFO("[MediaPipeline] Camera {} MediaMTX publisher disabled "
                 "(set VMS_ENABLE_MEDIAMTX=1 to enable WebRTC publish)", cfg_.camera_id);
        return true;                              // not an error — opt-in feature
    }
    if (mediamtx_) return true;                   // idempotent

    vms::CameraConfig pub_cfg;
    pub_cfg.mediamtx_url = cfg_.mediamtx_url;
    pub_cfg.ffmpeg_path  = "";

    vms::streaming::StreamProfile profile = vms::streaming::StreamProfile::createSub(cfg_.live_url);
    mediamtx_ = std::make_unique<vms::streaming::MediaMtxPublisher>(cfg_.camera_id, profile, pub_cfg);

    // QProcess + QTimer children created in start() must live on a thread that
    // owns a Qt event loop — the Qt main thread is the only safe choice today.
    auto* qt_main_thread = QCoreApplication::instance() ? QCoreApplication::instance()->thread() : nullptr;
    if (qt_main_thread) {
        mediamtx_->moveToThread(qt_main_thread);
    }

    bool started = false;
    auto* pub = mediamtx_.get();
    if (!qt_main_thread || QThread::currentThread() == qt_main_thread) {
        started = pub->start();
    } else {
        QMetaObject::invokeMethod(pub, [pub, &started]() {
            started = pub->start();
        }, Qt::BlockingQueuedConnection);
    }

    if (started) {
        LOG_INFO("[MediaPipeline] Camera {} MediaMTX publisher started -> {}",
                 cfg_.camera_id, mediamtx_->getPublishUrl().toStdString());
    } else {
        LOG_WARN("[MediaPipeline] Camera {} MediaMTX publisher failed to start (WebRTC disabled)",
                 cfg_.camera_id);
    }
    return started;
}

bool MediaPipeline::startContinuousRecorder() {
    if (continuous_) return true;
    continuous_ = std::make_unique<vms::recording::ContinuousRecorder>(
        cfg_.camera_id,
        cfg_.recording_url,
        "recordings",
        cfg_.segment_seconds,
        cfg_.retention_days);
    if (!continuous_->start()) {
        LOG_WARN("[MediaPipeline] Camera {} ContinuousRecorder failed to start", cfg_.camera_id);
        return false;
    }
    LOG_INFO("[MediaPipeline] Camera {} ContinuousRecorder started on {}",
             cfg_.camera_id, cfg_.recording_url);
    return true;
}

void MediaPipeline::stop() {
    if (stopped_) return;
    stopped_ = true;

    // Mirror the legacy ~PipelineContext stop ordering:
    //   buffer_pipeline → continuous_recorder → mediamtx_publisher.
    // Even though the producing worker is already joined by the caller, keeping
    // the same teardown sequence avoids any timing surprise inherited from a
    // dependent module.
    if (buffer_)     buffer_->stop();
    if (continuous_) continuous_->stop();
    if (mediamtx_)   mediamtx_->stop();
}

void MediaPipeline::writeRawData(const uint8_t* data, std::size_t size) {
    if (!buffer_ || data == nullptr || size == 0) return;
    buffer_->writeRawData(data, static_cast<int>(size));
}

std::string MediaPipeline::triggerEvent(const std::string& event_id,
                                       int duration_sec,
                                       int pre_record_sec) {
    if (!buffer_) return "";
    return buffer_->triggerEvent(event_id, duration_sec, pre_record_sec);
}

void MediaPipeline::connectToWorker(NativeReaderWorker* worker) {
    if (!worker || !mediamtx_) return;
    QObject::connect(worker, &vms::core::NativeReaderWorker::rawPacketReady,
                     mediamtx_.get(), &vms::streaming::MediaMtxPublisher::onPacketReady,
                     Qt::QueuedConnection);
}

std::string MediaPipeline::mediamtxPublishUrl() const {
    return mediamtx_ ? mediamtx_->getPublishUrl().toStdString() : std::string{};
}

} // namespace vms::core
