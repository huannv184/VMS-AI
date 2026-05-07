#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace vms {
namespace core { class NativeReaderWorker; }
namespace recording { class BufferPipeline; class ContinuousRecorder; }
namespace streaming { class MediaMtxPublisher; }
}

namespace vms::core {

// Aggregates the per-camera media subsystems that previously lived as 3 raw
// unique_ptr fields inside `PipelineContext`:
//   - BufferPipeline       : RAM circular buffer, fed by raw H264 packets,
//                             emits clip files on triggerEvent().
//   - MediaMtxPublisher    : opt-in FFmpeg-relay → MediaMTX → WebRTC WHEP.
//                             Owned on the Qt main thread (QProcess parent).
//   - ContinuousRecorder   : independent FFmpeg process pulling its own RTSP
//                             URL, segment-based 24/7 recording.
//
// Three separate start phases are exposed because the existing call site in
// CameraPipelineManager interleaves construction with worker setup:
//   Phase 1 — startBuffer()          : before worker creation
//                                       (so rawPacketReady→writeRawData is ready
//                                        as soon as the worker starts)
//   Phase 2 — startMediaMtx()        : after worker creation but before worker->start()
//   Phase 3 — startContinuousRecorder(): after worker->start() + 500ms staggered sleep
//                                       (avoids connection flood to Hikvision/Dahua
//                                        — see camera_pipeline_manager.cpp comment)
//
// stop() is idempotent and called by the destructor; tears down in the order
// buffer → continuous_recorder → mediamtx (preserves the legacy ~PipelineContext
// ordering, even though once the producing worker is joined the ordering does
// not affect correctness — keeping it identical avoids destabilising any timing
// assumption a downstream component may have inherited).
class MediaPipeline {
public:
    struct Config {
        int         camera_id{-1};
        std::string live_url;          // Main stream — buffer + mediamtx feed
        std::string recording_url;     // Sub stream when present, else main — continuous_recorder pull
        int         segment_seconds{60};
        int         retention_days{7};
        bool        mediamtx_enabled{false};
        std::string mediamtx_url;      // Empty = MediaMtxPublisher uses its default
    };

    explicit MediaPipeline(Config cfg);
    ~MediaPipeline();

    MediaPipeline(const MediaPipeline&)            = delete;
    MediaPipeline& operator=(const MediaPipeline&) = delete;
    MediaPipeline(MediaPipeline&&)                 = delete;
    MediaPipeline& operator=(MediaPipeline&&)      = delete;

    // Phase 1 — required. Returns false if BufferPipeline failed to start
    // (caller may continue without recording-on-event).
    bool startBuffer();

    // Phase 2 — opt-in. No-op (returns true) if cfg.mediamtx_enabled == false.
    // Creates the publisher on the Qt main thread; safe to call from any thread.
    bool startMediaMtx();

    // Phase 3 — required for 24/7 recording. Independent FFmpeg subprocess.
    // Call AFTER worker->start() to preserve staggered cam-connection ordering.
    bool startContinuousRecorder();

    // Idempotent. Stops in declared-correct order. Called by destructor.
    void stop();

    // Hot path: forward a raw H264 packet to the circular buffer. Safe to call
    // concurrently with everything except start/stop. Internally, BufferPipeline
    // synchronises its own writes.
    void writeRawData(const uint8_t* data, std::size_t size);

    // Trigger event-clip extraction from the circular buffer. Returns the path
    // to the produced video file or "" on failure (no buffer or extraction error).
    std::string triggerEvent(const std::string& event_id,
                             int duration_sec   = 20,
                             int pre_record_sec = 10);

    // Connect the worker's rawPacketReady signal to the MediaMTX publisher's
    // onPacketReady slot. No-op if MediaMTX is disabled or hasn't started.
    // The connection is QueuedConnection (cross-thread) and lives until either
    // side is destroyed — Qt cleans it up in MediaMtxPublisher::~MediaMtxPublisher.
    void connectToWorker(NativeReaderWorker* worker);

    // Diagnostic accessor used by [Manager] log line on successful publisher start.
    std::string mediamtxPublishUrl() const;

private:
    Config cfg_;
    std::unique_ptr<vms::recording::BufferPipeline>      buffer_;
    std::unique_ptr<vms::streaming::MediaMtxPublisher>   mediamtx_;
    std::unique_ptr<vms::recording::ContinuousRecorder>  continuous_;
    bool stopped_{false};
};

} // namespace vms::core
