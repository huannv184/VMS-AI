#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <queue>
#include <vector>
#include <functional>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>

namespace vms {
namespace core {

// 2026-05-20 PPE compliance aggregator. Rolling 60-minute window per
// camera of (compliant_ticks, violating_ticks) — one tick per
// PPE-person evaluated per frame. ai_worker emits the per-frame counts
// in metadata["ppe_summary"]; eventWorkerLoop calls recordTick().
// REST endpoint (analytics_controller) serializes via snapshot().
//
// Storage: per-camera ring of MinuteSlot keyed by (epoch_minute % 60).
// On insert: if slot's stored epoch_minute != current → reset slot (60
// minutes have wrapped). Compute window N by walking up to N newest
// slots. Single mutex; data is small (~64 cameras × 60 slots × 24 B =
// 92 KB worst-case). recordTick called from event_worker threads (2
// of them) and snapshot from HTTP handlers — low contention.
class PpeComplianceAggregator {
public:
    static PpeComplianceAggregator& getInstance();

    void recordTick(int camera_id,
                    int64_t ts_ms,
                    int compliant_count,
                    int violating_count);

    // window_minutes clamped to [1, 60]. Returns:
    //   { window_minutes, generated_at_ms,
    //     global:    { compliant_ticks, violating_ticks, compliance_rate, samples },
    //     per_camera:{ "<id>": { compliant_ticks, violating_ticks, compliance_rate, samples }, ... } }
    nlohmann::json snapshot(int window_minutes) const;

    // 2026-05-25 PR-2 readiness probes. Atomic loads — safe to call from
    // HTTP handlers without contending with recordTick on the data path.
    // last_tick_ms() = 0 means the aggregator has NEVER recorded a tick
    // since process start (vs "ticked an hour ago, ring slot has wrapped").
    int64_t lastTickMs()      const { return last_tick_ms_.load(std::memory_order_relaxed); }
    int64_t lastViolationMs() const { return last_violation_ms_.load(std::memory_order_relaxed); }

    // Number of cameras that have ticked at least once since process
    // start. Briefly takes the mutex; cheap at /status poll rates.
    std::size_t cameraCount() const;

private:
    PpeComplianceAggregator() = default;
    struct MinuteSlot {
        int64_t epoch_minute{-1}; // -1 = unused
        uint64_t compliant_ticks{0};
        uint64_t violating_ticks{0};
    };
    static constexpr int kSlotCount = 60;
    using CameraRing = std::array<MinuteSlot, kSlotCount>;
    std::unordered_map<int, CameraRing> rings_;
    mutable std::mutex mtx_;

    // 2026-05-25 PR-2: behavioral readiness signals. Atomic so /status
    // can read them without acquiring mtx_ (which the data path holds
    // on every PPE-bearing frame). last_violation_ms_ stays at the value
    // of the most recent recordTick where violating_count > 0; it never
    // decreases — operator UI shows "X seconds since last violation".
    std::atomic<int64_t> last_tick_ms_{0};
    std::atomic<int64_t> last_violation_ms_{0};
};

class AiEventProcessor {
public:
    static AiEventProcessor& getInstance();

    AiEventProcessor();
    ~AiEventProcessor();

    void processMetadata(int camera_id, const nlohmann::json& metadata, const cv::Mat& frame);

    // 2026-05-22 Path-1 per-camera override for the multi-frame track
    // confirmation gate. Called by CameraPipelineManager when starting
    // (or restarting) a pipeline; reads the value from the camera's
    // ai_config JSON column. Clamped to [1, 30]; <=0 removes the
    // per-camera entry (falls back to env / kTrackConfirmCount). Lookup
    // in observeTrackAndCheckConfirmed is O(1) hash + mutex-guarded.
    void setCameraConfirmCount(int camera_id, int count);
    void clearCameraConfirmCount(int camera_id);

private:
    // ── Event processing: bounded worker pool with joinable workers ───────────
    // Previously processMetadata() spawned std::thread(...).detach() with `this`
    // captured. Detached workers held a pointer to the singleton without any
    // synchronisation against destruction — at process shutdown the destructor
    // would join only the upload worker and return, while detached workers were
    // still mid-flight reading cooldown_cache_/upload_mutex_/etc. That's a
    // textbook race / use-after-mutex-destruction. Replaced with N workers that
    // drain a bounded queue and are joined in the destructor under event_stop_.
    static constexpr int MAX_EVENT_THREADS = 2;
    static constexpr int MAX_EVENT_QUEUE = 16; // bounded backpressure: drop overflow
    struct EventJob {
        int camera_id{0};
        nlohmann::json metadata;
        cv::Mat frame;       // owned clone — safe across thread boundary
        int64_t ts_ms{0};
    };
    std::queue<EventJob> event_queue_;
    std::mutex event_queue_mutex_;
    std::condition_variable event_queue_cv_;
    std::vector<std::thread> event_workers_;
    bool event_stop_{false};
    void eventWorkerLoop();

    // ── Async upload queue (single worker thread, bounded to 32 tasks) ────────
    static constexpr int MAX_UPLOAD_QUEUE = 32;
    struct UploadTask { std::string local_path; std::string object_key; };
    std::queue<UploadTask> upload_queue_;
    std::mutex upload_mutex_;
    std::condition_variable upload_cv_;
    std::thread upload_worker_;
    bool upload_stop_{false};
    void uploadWorkerLoop();
    void enqueueUpload(std::string local_path, std::string object_key);

    // ── Detection cooldown ────────────────────────────────────────────────────
    std::unordered_map<std::string, std::chrono::system_clock::time_point> cooldown_cache_;
    std::mutex cache_mutex_;
    static constexpr int COOLDOWN_SECONDS = 10;
    static constexpr int MAX_COOLDOWN_CACHE = 1000;

    // ── 2026-05-21 Path-1 — Multi-frame track confirmation ────────────────────
    // Operator complaint: dogs / shadows / transient blobs that COCO YOLO
    // briefly classifies as "person" still fire an event on the SINGLE
    // first frame — by the time the tracker drops them as ephemeral,
    // damage is done (event row, snapshot, push). Required N consecutive
    // tracked observations before promoting any track_id to an Event.
    // Real persons consistently re-detect every frame for their dwell
    // time; single-frame hallucinations don't.
    //
    // State: per-(camera, track) observation count + last-seen ts. Lazy
    // GC drops entries idle for > kTrackObsTTLSec on each insert when
    // the map grows past a soft cap. Map written by event_workers (2
    // threads) so mutex-guarded.
    //
    // Bypass: track_id == -1 (tracker disabled / PPE-only path) skips
    // the confirmation check — the position-bucket cooldown still
    // gates re-fires. Aspect-ratio + min-height filters at ai_worker
    // already cut most FPs at the parser level when tracker is off.
    struct TrackObservation {
        int count{0};
        std::chrono::steady_clock::time_point last_seen;
    };
    std::unordered_map<std::string, TrackObservation> track_observations_;
    // Per-camera override map, populated by setCameraConfirmCount. Read
    // path in observeTrackAndCheckConfirmed; both sides hold
    // track_obs_mutex_. Keeping this under the same mutex (vs. a
    // separate one) sidesteps lock-ordering concerns and the map is
    // touched once per Event candidate frame — contention is bounded
    // by the existing 2 event_worker threads.
    std::unordered_map<int, int> per_camera_confirm_count_;
    std::mutex track_obs_mutex_;
    static constexpr int kTrackConfirmCount = 3;       // override via VMS_INTRUSION_CONFIRM_FRAMES or ai_config.intrusion_confirm_frames
    static constexpr int kTrackObsTTLSec = 30;
    static constexpr size_t kTrackObsSoftCap = 5000;   // GC trigger
    // Returns true when this observation should produce an Event (count
    // >= confirm threshold OR track_id == -1 bypass). Always increments
    // the per-track counter.
    bool observeTrackAndCheckConfirmed(int camera_id, int track_id);

    // Accepts pre-decoded frame to avoid repeated imdecode per object
    void processFace(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    void processIntrusion(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    void processLicensePlate(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    // 2026-05-19 PPE violation handler. Triggered by class_ids 302
    // (NoHelmet) and 304 (NoVest) from the PPE-YOLOv8 engine (ai_worker
    // remaps engine's 0-5 → 300-305 to avoid collision with COCO
    // person=0). Emits an Event row of type PPE_VIOLATION with snapshot
    // + cooldown by camera+location.
    void processPPEViolation(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    // BUG-REID-DEAD-PIPELINE producer: cross-camera ReID enrichment for every
    // person detection with a stable track_id. Engine handles its own cache
    // (track_to_global_) so per-frame calls short-circuit cheaply on known
    // tracks; only the first observation of a (camera, track) pair runs the
    // DNN embedding extraction.
    void processReID(int camera_id, const nlohmann::json& obj, const cv::Mat& frame);
    // Batched line-crossing pass — runs once per metadata batch, after the
    // per-object loop has populated the TrackerStateManager. Builds the
    // detection list internally so the tracker only advances once per frame.
    void processLineCrossings(int camera_id,
                              const nlohmann::json& metadata,
                              const cv::Mat& frame,
                              int64_t ts_ms);
    bool isOnCooldown(const std::string& key);
    void setCooldown(const std::string& key);
    cv::Mat cropSnapshot(const cv::Mat& frame, const nlohmann::json& bbox);
    std::string saveSnapshot(int camera_id, const cv::Mat& crop);
};

} // namespace core
} // namespace vms
