// ==============================================================
// File: src/core/ppe_compliance_aggregator.cpp
//
// Impl extracted from ai_event_processor.cpp on 2026-05-25
// (PR-7B refactor — behaviour-preserving). Code moved verbatim;
// only the file location changed. See ppe_compliance_aggregator.h
// for the rationale (decouple from AiEventProcessor's heavy
// dependency graph so integration tests can link the class
// directly).
// ==============================================================

#include "core/ppe_compliance_aggregator.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace vms {
namespace core {

PpeComplianceAggregator& PpeComplianceAggregator::getInstance() {
    static PpeComplianceAggregator inst;
    return inst;
}

void PpeComplianceAggregator::recordTick(int camera_id,
                                         int64_t ts_ms,
                                         int compliant_count,
                                         int violating_count) {
    if (compliant_count <= 0 && violating_count <= 0) return;
    const int64_t minute = ts_ms / 60000;
    const size_t idx = static_cast<size_t>(((minute % kSlotCount) + kSlotCount) % kSlotCount);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& ring = rings_[camera_id];
        auto& slot = ring[idx];
        if (slot.epoch_minute != minute) {
            // Slot is stale (60 minutes have wrapped past it OR first write).
            // Reset the bucket before accumulating into the new minute.
            slot.epoch_minute    = minute;
            slot.compliant_ticks = 0;
            slot.violating_ticks = 0;
        }
        if (compliant_count > 0) slot.compliant_ticks += static_cast<uint64_t>(compliant_count);
        if (violating_count > 0) slot.violating_ticks += static_cast<uint64_t>(violating_count);
    }

    // 2026-05-25 PR-2: readiness signals updated after the slot write so a
    // reader that sees a non-zero last_tick_ms_ is guaranteed to find the
    // corresponding tick in the ring on a subsequent snapshot() — relaxed
    // ordering is fine because /status doesn't pair these atomics with
    // ring-state assertions, it just reports the wall-clock recency.
    last_tick_ms_.store(ts_ms, std::memory_order_relaxed);
    if (violating_count > 0) {
        last_violation_ms_.store(ts_ms, std::memory_order_relaxed);
    }
}

std::size_t PpeComplianceAggregator::cameraCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return rings_.size();
}

nlohmann::json PpeComplianceAggregator::snapshot(int window_minutes) const {
    if (window_minutes < 1)  window_minutes = 1;
    if (window_minutes > kSlotCount) window_minutes = kSlotCount;

    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t now_minute = now_ms / 60000;
    const int64_t window_start_minute = now_minute - (window_minutes - 1);

    nlohmann::json per_camera = nlohmann::json::object();
    uint64_t global_compliant = 0;
    uint64_t global_violating = 0;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& [cam_id, ring] : rings_) {
            uint64_t c = 0, v = 0;
            int samples = 0;
            for (const auto& slot : ring) {
                if (slot.epoch_minute < window_start_minute) continue;
                if (slot.epoch_minute > now_minute)         continue;
                c += slot.compliant_ticks;
                v += slot.violating_ticks;
                if (slot.compliant_ticks + slot.violating_ticks > 0) ++samples;
            }
            const uint64_t total = c + v;
            double rate = (total > 0) ? (static_cast<double>(c) / static_cast<double>(total)) : 1.0;
            per_camera[std::to_string(cam_id)] = {
                {"compliant_ticks", c},
                {"violating_ticks", v},
                {"compliance_rate", rate},
                {"samples",         samples}
            };
            global_compliant += c;
            global_violating += v;
        }
    }

    const uint64_t global_total = global_compliant + global_violating;
    const double global_rate = (global_total > 0)
        ? (static_cast<double>(global_compliant) / static_cast<double>(global_total))
        : 1.0;
    return {
        {"window_minutes",  window_minutes},
        {"generated_at_ms", now_ms},
        {"global", {
            {"compliant_ticks", global_compliant},
            {"violating_ticks", global_violating},
            {"compliance_rate", global_rate}
        }},
        {"per_camera", per_camera}
    };
}

} // namespace core
} // namespace vms
