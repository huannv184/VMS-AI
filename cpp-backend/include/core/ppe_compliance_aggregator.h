// ==============================================================
// File: include/core/ppe_compliance_aggregator.h
//
// Rolling 60-minute PPE compliance aggregator. Extracted from
// ai_event_processor.h on 2026-05-25 (PR-7B refactor — behaviour-
// preserving) so the class can be linked into integration tests
// without dragging in the AiEventProcessor entanglement
// (cv::Mat + TrackerStateManager + EventManager + DbManager).
//
// 2026-05-20 PPE compliance aggregator. Per-camera ring of
// (compliant_ticks, violating_ticks) — one tick per PPE-person
// evaluated per frame. ai_worker emits the per-frame counts in
// metadata["ppe_summary"]; AiEventProcessor::eventWorkerLoop
// calls recordTick(). REST endpoint (analytics_controller)
// serializes via snapshot().
//
// Storage: per-camera ring of MinuteSlot keyed by
// (epoch_minute % 60). On insert: if slot's stored epoch_minute
// != current → reset slot (60 minutes have wrapped). Compute
// window N by walking up to N newest slots. Single mutex; data
// is small (~64 cameras × 60 slots × 24 B = 92 KB worst-case).
// recordTick called from event_worker threads (2 of them) and
// snapshot from HTTP handlers — low contention.
// ==============================================================

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace vms {
namespace core {

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

} // namespace core
} // namespace vms
