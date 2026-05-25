// ==============================================================
// File: include/core/attendance_tracker.h
// AttendanceTracker — debounces face_recognized events into
// attendance rows. One row per (person, camera) per cooldown
// window (default 60s, env VMS_ATTENDANCE_DEDUP_SECONDS).
//
// Resolves event "kind" from camera_roles (entry / exit / both /
// observe). Resolves employee_id from in-memory cache populated
// from `employees` table (refreshed on CRUD via reloadEmployees()).
// ==============================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// BulkWriter is template — forward-declare so this header stays Qt-free.
// The header's "tests link via header alone" intent (see comment block
// below) breaks if we pull bulk_writer.h, which transitively brings in
// <QSqlDatabase>. unique_ptr<BulkWriter<Row>> only needs the complete
// type at the destructor's point of use; attendance_tracker.cpp owns
// that out-of-line and includes the real header.
namespace vms::utils { template <typename> class BulkWriter; }

namespace vms::core {

// ── Pure dedup decision helpers ──────────────────────────────────────────
// Inline so tests link them via header alone — no need to compile the rest
// of attendance_tracker.cpp (which pulls Qt SQL + DbManager + BulkWriter).
// onFaceRecognized + resolveKind below use these so tests and prod cannot
// drift apart silently.

// True if (now - prev) ≥ cooldown — i.e. punch should be accepted.
inline bool isOutsideCooldownWindow(int64_t prev_ts_s,
                                    int64_t now_ts_s,
                                    int cooldown_s) noexcept {
    return (now_ts_s - prev_ts_s) >= static_cast<int64_t>(cooldown_s);
}

// Pure role → kind resolver. last_kind only matters for the "both"
// (toggling) role; for "entry"/"exit" the kind is fixed; for everything
// else the camera has no door semantics so we emit "seen".
inline std::string kindForRole(const std::string& role,
                               const std::string& last_kind) {
    if (role == "entry") return "in";
    if (role == "exit")  return "out";
    if (role == "both")  return (last_kind == "in") ? "out" : "in";
    return "seen";
}

struct AttendanceRow {
    int          person_id    = -1;     // -1 if unknown
    int          employee_id  = -1;     // -1 if not linked
    int          camera_id    = -1;
    std::string  kind;                  // "in" | "out" | "seen"
    int64_t      timestamp_s  = 0;      // epoch seconds
    float        confidence   = 0.0f;
    std::string  snapshot_path;
    std::string  source_rule;           // "door_role" | "min_max_fallback" | "manual"
    std::string  metadata_json;         // optional JSON blob
};

class AttendanceTracker {
public:
    static AttendanceTracker& getInstance();

    // Lifecycle (idempotent).
    void start();
    void stop();

    // Hot path — called from AiEventProcessor::processFace.
    // Drops if person_id <= 0 or within cooldown for (person, camera).
    void onFaceRecognized(int camera_id,
                          int person_id,
                          float confidence,
                          int64_t ts_seconds,
                          const std::string& snapshot_path);

    // Manual punch (called from AttendanceController POST /v2/manual).
    // Bypasses cooldown; source_rule="manual".
    bool recordManual(int employee_id,
                      int camera_id,
                      const std::string& kind,
                      int64_t ts_seconds,
                      std::string* err = nullptr);

    // Cache management — called by AttendanceController on CRUD.
    void reloadEmployees();
    void reloadCameraRoles();

    // Health.
    std::size_t pendingRows();
    std::uint64_t droppedRows();

    // 2026-05-25 PR-5 audit endpoint probes. Atomic loads — safe from
    // HTTP handlers without contending with the data path.
    // lastEventTs() = 0 means the tracker has NEVER accepted a recognized
    // face since process start (vs "ticked, then went quiet").
    bool          started() const     { return started_.load(std::memory_order_acquire); }
    int64_t       lastEventTs() const { return last_event_ts_.load(std::memory_order_relaxed); }

    // Cache sizes — briefly lock cache_mu_. Cheap at /health poll rates.
    // employeesCached == 0 means /api/attendance/employees was never
    // populated OR the cache hasn't been reloaded after a CRUD op.
    std::size_t   employeesCached();
    std::size_t   cameraRolesCached();

    // Pure key-pack — exposed for unit test (test_attendance_dedup.cpp
    // exercises the real packing rule so prod + test cannot drift).
    // person_id (int32) ⊕ camera_id (int32) packed into uint64 for dedup map.
    static inline uint64_t packKey(int person_id, int camera_id) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(person_id)) << 32)
             |  static_cast<uint64_t>(static_cast<uint32_t>(camera_id));
    }

private:
    AttendanceTracker();
    ~AttendanceTracker();
    AttendanceTracker(const AttendanceTracker&) = delete;
    AttendanceTracker& operator=(const AttendanceTracker&) = delete;

    std::string resolveKind(int camera_id, uint64_t key);
    int         resolveEmployeeId(int person_id);

    int                                cooldown_seconds_;  // from env, default 60
    std::atomic<bool>                  started_{false};

    // 2026-05-25 PR-5: behavioural readiness signal. Bumped in
    // onFaceRecognized (after dedup pass) and recordManual; relaxed
    // ordering — /health reads this for "seconds since last event" only.
    std::atomic<int64_t>               last_event_ts_{0};

    // Dedup state — small (≤ persons * cameras), GC opportunistically.
    struct DedupEntry { int64_t last_ts_s; std::string last_kind; };
    std::mutex                                       dedup_mu_;
    std::unordered_map<uint64_t, DedupEntry>         dedup_;

    // Caches refreshed on CRUD.
    std::mutex                                       cache_mu_;
    std::unordered_map<int /*person_id*/, int /*employee_id*/> emp_by_person_;
    std::unordered_map<int /*camera_id*/, std::string /*role*/> camera_role_;

    std::unique_ptr<vms::utils::BulkWriter<AttendanceRow>> writer_;
};

}  // namespace vms::core
