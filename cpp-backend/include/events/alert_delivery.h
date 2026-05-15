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
#include <string>

namespace vms::events {

// Forward decls — full defs in rule_engine.h
struct CompositeRule;
struct RuleAction;

// All delivery is async (queued to a shared bounded background runner with
// drop-on-full). Safe to call from any thread, including the
// EventManager::createEvent hot path. Returns immediately; failures are
// logged inside the worker.
void deliverAction(const CompositeRule& rule,
                   const RuleAction& action,
                   const RawEvent& event);

// Stop the internal background runner. Call once during graceful shutdown
// AFTER the last possible producer (EventManager / RuleEngine / brand
// event services) has stopped.
void shutdownDelivery();

} // namespace vms::events
