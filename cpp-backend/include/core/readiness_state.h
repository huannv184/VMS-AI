#pragma once

// Production-grade readiness model. Split from the legacy `/api/health`
// endpoint which conflated liveness and readiness:
//
//   * Liveness  = "process is alive". Used by orchestrators (k8s, NSSM,
//     systemd) to decide whether to KILL+RESTART. Should never depend on
//     external state — a flaky DB shouldn't cause us to restart, only to
//     drop out of load-balancer rotation.
//
//   * Readiness = "process can serve real traffic". Used by load balancers
//     to decide whether to ROUTE TRAFFIC. Must reflect all required
//     downstream dependencies (DB always required; storage gated on a
//     deployment policy decision, see ReadinessSnapshot::storage_required).
//
// This header is pure — no Crow, no atomics, no StorageManager include.
// Callers fill a ReadinessSnapshot from their globals (atomic loads in
// the route handler) and pass it to computeReady. That keeps the policy
// logic unit-testable without spinning up the whole server.
//
// Storage policy is intentionally decoupled. The current default is
// `storage_required = false` (storage outage is informational, doesn't
// fail readiness) so this commit doesn't preempt the storage-resilience
// P0 follow-up. When that work lands, it can flip the snapshot field
// based on config or a new atomic without touching this header or the
// readiness tests.

namespace vms {
namespace core {

struct ReadinessSnapshot {
    bool db_ready{false};
    bool storage_ready{false};
    bool storage_required{false};
};

// Pure function — no side effects, deterministic per input.
// DB is always treated as required (the backend cannot serve any
// meaningful API without it). Storage is required only when the
// snapshot says so.
inline bool computeReady(const ReadinessSnapshot& s) {
    if (!s.db_ready) return false;
    if (s.storage_required && !s.storage_ready) return false;
    return true;
}

} // namespace core
} // namespace vms
