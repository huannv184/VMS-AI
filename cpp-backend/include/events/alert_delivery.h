// ==============================================================
// File: include/events/alert_delivery.h
// Single delivery layer for RuleEngine actions (replaces the
// AlertManager + AlertRouter dual stack).
//
// Pre-2026-05-14 the codebase had three rule layers:
//   1. legacy `alert_rules` table → AlertManager::processEvent → libcurl
//   2. modern `rules` table → RuleEngine::executeActions → AlertRouter
//   3. AlertRouter's in-memory rules_ list (NEVER populated, all ALERT/
//      WEBHOOK actions silently dropped)
// (1) was the only path that actually delivered, (2) was a no-op because
// AlertRouter::addRule had zero callers, (3) was dead code.
//
// This module replaces (2)+(3) with direct delivery from
// RuleEngine::executeActions and removes (1) entirely. CompositeRule
// + RuleAction stays canonical; RuleAction.metadata carries per-channel
// recipients (email_addresses[], phone_numbers[], telegram_chat_id).
// ==============================================================

#pragma once

#include "events/event_types.h"
#include <nlohmann/json.hpp>
#include <string>

namespace vms::events {

// Forward decls — full defs in rule_engine.h
struct CompositeRule;
struct RuleAction;

// All delivery is async (queued to per-channel bounded background runners
// with drop-on-full). Safe to call from any thread, including the
// EventManager::createEvent hot path. Returns immediately; failures are
// logged inside the worker. Per-channel pools (webhook / sms / telegram /
// alarm) are independent — a stalled webhook endpoint does NOT delay
// telegram/SMS/alarm delivery (2026-05-17 BUG-ALERT-CASCADE-POOL-01 fix).
void deliverAction(const CompositeRule& rule,
                   const RuleAction& action,
                   const RawEvent& event);

// Snapshot of per-channel runner counters. Returns:
//   { webhook: { name, worker_count, max_queue_size, current_queue_depth,
//                submitted_total, dropped_total, peak_queue_depth, stopping },
//     sms:      { … same shape … },
//     telegram: { … },
//     alarm:    { … },
//     aggregate:{ submitted_total, dropped_total, peak_queue_depth } }
// `aggregate` is a roll-up (sum for totals, max for peak) so dashboards
// have a single "how many alerts did we drop" number alongside the
// per-channel breakdown. Surfaced on `GET /api/rules/stats` (ALERT_READ).
// Wait-free w.r.t. producer hot path — pure atomic loads + try_lock.
nlohmann::json deliveryStats();

// Stop all per-channel runners. Call once during graceful shutdown AFTER
// the last possible producer (EventManager / RuleEngine / brand event
// services) has stopped. Pools are independent — calls run sequentially
// but each pool's workers drain in parallel.
void shutdownDelivery();

} // namespace vms::events
