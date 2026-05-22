// ==============================================================
// File: src/events/rule_engine.cpp
// Composite Rule Engine Implementation
// ==============================================================

#include "events/rule_engine.h"
#include "events/alert_delivery.h"
#include "utils/logger.h"
#include "database/db_manager.h"
#include "core/camera_pipeline_manager.h"
#include "core/snapshot_manager.h"
#include <algorithm>
#include <ctime>

#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QString>

namespace vms::events {

// ============================================================================
// ALERT CHANNEL helpers — moved from the deleted alert_router.cpp.
// ============================================================================

const char* alertChannelToString(AlertChannel channel) {
    switch (channel) {
        case AlertChannel::UI_NOTIFICATION: return "UI_NOTIFICATION";
        case AlertChannel::EMAIL:           return "EMAIL";
        case AlertChannel::SMS:             return "SMS";
        case AlertChannel::WEBHOOK:         return "WEBHOOK";
        case AlertChannel::MOBILE_PUSH:     return "MOBILE_PUSH";
        case AlertChannel::ALARM_OUTPUT:    return "ALARM_OUTPUT";
        default:                            return "NONE";
    }
}

AlertChannel stringToAlertChannel(const std::string& str) {
    if (str == "UI_NOTIFICATION") return AlertChannel::UI_NOTIFICATION;
    if (str == "EMAIL")           return AlertChannel::EMAIL;
    if (str == "SMS")             return AlertChannel::SMS;
    if (str == "WEBHOOK")         return AlertChannel::WEBHOOK;
    if (str == "MOBILE_PUSH")     return AlertChannel::MOBILE_PUSH;
    if (str == "ALARM_OUTPUT")    return AlertChannel::ALARM_OUTPUT;
    return AlertChannel::NONE;
}

// ============================================================================
// SERIALIZATION â€” RuleCondition
// ============================================================================

static const char* conditionTypeToString(ConditionType t) {
    switch (t) {
        case ConditionType::OBJECT: return "OBJECT";
        case ConditionType::ZONE: return "ZONE";
        case ConditionType::TIME: return "TIME";
        case ConditionType::BEHAVIOR: return "BEHAVIOR";
        case ConditionType::EVENT_TYPE: return "EVENT_TYPE";
        case ConditionType::SEVERITY: return "SEVERITY";
        case ConditionType::CONFIDENCE: return "CONFIDENCE";
        case ConditionType::CAMERA: return "CAMERA";
        case ConditionType::RULE_TRIGGERED: return "RULE_TRIGGERED";
        default: return "UNKNOWN";
    }
}

static ConditionType stringToConditionType(const std::string& s) {
    if (s == "OBJECT") return ConditionType::OBJECT;
    if (s == "ZONE") return ConditionType::ZONE;
    if (s == "TIME") return ConditionType::TIME;
    if (s == "BEHAVIOR") return ConditionType::BEHAVIOR;
    if (s == "EVENT_TYPE") return ConditionType::EVENT_TYPE;
    if (s == "SEVERITY") return ConditionType::SEVERITY;
    if (s == "CONFIDENCE") return ConditionType::CONFIDENCE;
    if (s == "CAMERA") return ConditionType::CAMERA;
    if (s == "RULE_TRIGGERED") return ConditionType::RULE_TRIGGERED;
    return ConditionType::OBJECT;
}

nlohmann::json RuleCondition::toJSON() const {
    nlohmann::json j;
    j["type"] = conditionTypeToString(type);
    j["object_class"] = object_class;
    j["zone_id"] = zone_id;
    j["zone_action"] = zone_action;
    j["start_hour"] = start_hour;
    j["end_hour"] = end_hour;
    j["days_of_week"] = days_of_week;
    j["behavior_type"] = eventTypeToString(behavior_type);
    j["duration_sec"] = duration_sec;
    
    nlohmann::json et_arr = nlohmann::json::array();
    for (auto t : event_types) et_arr.push_back(eventTypeToString(t));
    j["event_types"] = et_arr;
    
    j["min_severity"] = eventSeverityToString(min_severity);
    j["min_confidence"] = min_confidence;
    j["camera_ids"] = camera_ids;
    j["referenced_rule_id"] = referenced_rule_id;
    j["chaining_window_sec"] = chaining_window_sec;
    return j;
}

RuleCondition RuleCondition::fromJSON(const nlohmann::json& j) {
    RuleCondition c;
    c.type = stringToConditionType(j.value("type", "OBJECT"));
    c.object_class = j.value("object_class", "");
    c.zone_id = j.value("zone_id", -1);
    c.zone_action = j.value("zone_action", "enter");
    c.start_hour = j.value("start_hour", 0);
    c.end_hour = j.value("end_hour", 23);
    if (j.contains("days_of_week")) c.days_of_week = j["days_of_week"].get<std::vector<int>>();
    c.behavior_type = stringToEventType(j.value("behavior_type", "UNKNOWN"));
    c.duration_sec = j.value("duration_sec", 0);
    if (j.contains("event_types") && j["event_types"].is_array()) {
        for (auto& s : j["event_types"]) c.event_types.push_back(stringToEventType(s));
    }
    c.min_severity = stringToEventSeverity(j.value("min_severity", "LOW"));
    c.min_confidence = j.value("min_confidence", 0.0f);
    if (j.contains("camera_ids")) c.camera_ids = j["camera_ids"].get<std::vector<int>>();
    c.referenced_rule_id = j.value("referenced_rule_id", -1);
    c.chaining_window_sec = j.value("chaining_window_sec", 30);
    return c;
}

// ============================================================================
// SERIALIZATION â€” RuleAction
// ============================================================================

static const char* actionTypeToString(ActionType t) {
    switch (t) {
        case ActionType::ALERT: return "ALERT";
        case ActionType::RECORD_CLIP: return "RECORD_CLIP";
        case ActionType::SNAPSHOT: return "SNAPSHOT";
        case ActionType::WEBHOOK: return "WEBHOOK";
        case ActionType::LOG: return "LOG";
        default: return "LOG";
    }
}

static ActionType stringToActionType(const std::string& s) {
    if (s == "ALERT") return ActionType::ALERT;
    if (s == "RECORD_CLIP") return ActionType::RECORD_CLIP;
    if (s == "SNAPSHOT") return ActionType::SNAPSHOT;
    if (s == "WEBHOOK") return ActionType::WEBHOOK;
    return ActionType::LOG;
}

nlohmann::json RuleAction::toJSON() const {
    nlohmann::json j;
    j["type"] = actionTypeToString(type);
    nlohmann::json ch_arr = nlohmann::json::array();
    for (auto c : channels) ch_arr.push_back(alertChannelToString(c));
    j["channels"] = ch_arr;
    j["pre_event_sec"] = pre_event_sec;
    j["post_event_sec"] = post_event_sec;
    j["webhook_url"] = webhook_url;
    j["metadata"] = metadata;
    return j;
}

RuleAction RuleAction::fromJSON(const nlohmann::json& j) {
    RuleAction a;
    a.type = stringToActionType(j.value("type", "LOG"));
    if (j.contains("channels") && j["channels"].is_array()) {
        for (auto& s : j["channels"]) a.channels.push_back(stringToAlertChannel(s));
    }
    a.pre_event_sec = j.value("pre_event_sec", 10);
    a.post_event_sec = j.value("post_event_sec", 30);
    a.webhook_url = j.value("webhook_url", "");
    if (j.contains("metadata")) a.metadata = j["metadata"];
    return a;
}

// ============================================================================
// SERIALIZATION â€” AntiNoiseConfig
// ============================================================================

nlohmann::json AntiNoiseConfig::toJSON() const {
    return {
        {"cooldown_sec", cooldown_sec},
        {"debounce_sec", debounce_sec},
        {"min_duration_sec", min_duration_sec},
        {"confidence_threshold", confidence_threshold},
        {"max_alerts_per_hour", max_alerts_per_hour}
    };
}

AntiNoiseConfig AntiNoiseConfig::fromJSON(const nlohmann::json& j) {
    AntiNoiseConfig c;
    c.cooldown_sec = j.value("cooldown_sec", 60);
    c.debounce_sec = j.value("debounce_sec", 3);
    c.min_duration_sec = j.value("min_duration_sec", 2);
    c.confidence_threshold = j.value("confidence_threshold", 0.5f);
    c.max_alerts_per_hour = j.value("max_alerts_per_hour", 60);
    return c;
}

// ============================================================================
// SERIALIZATION â€” CompositeRule
// ============================================================================

nlohmann::json CompositeRule::toJSON() const {
    nlohmann::json j;
    j["rule_id"] = rule_id;
    j["name"] = name;
    j["description"] = description;
    j["enabled"] = enabled;
    j["logic"] = (logic == RuleLogic::AND) ? "AND" : "OR";
    
    nlohmann::json conds = nlohmann::json::array();
    for (auto& c : conditions) conds.push_back(c.toJSON());
    j["conditions"] = conds;
    
    nlohmann::json acts = nlohmann::json::array();
    for (auto& a : actions) acts.push_back(a.toJSON());
    j["actions"] = acts;
    
    j["anti_noise"] = anti_noise.toJSON();
    j["camera_ids"] = camera_ids;
    j["metadata"] = metadata;
    j["created_at"] = std::chrono::system_clock::to_time_t(created_at);
    j["updated_at"] = std::chrono::system_clock::to_time_t(updated_at);
    
    return j;
}

CompositeRule CompositeRule::fromJSON(const nlohmann::json& j) {
    CompositeRule r;
    r.rule_id = j.value("rule_id", 0);
    r.name = j.value("name", "");
    r.description = j.value("description", "");
    r.enabled = j.value("enabled", true);
    r.logic = (j.value("logic", "AND") == "OR") ? RuleLogic::OR : RuleLogic::AND;
    
    if (j.contains("conditions") && j["conditions"].is_array()) {
        for (auto& c : j["conditions"]) r.conditions.push_back(RuleCondition::fromJSON(c));
    }
    if (j.contains("actions") && j["actions"].is_array()) {
        for (auto& a : j["actions"]) r.actions.push_back(RuleAction::fromJSON(a));
    }
    if (j.contains("anti_noise")) r.anti_noise = AntiNoiseConfig::fromJSON(j["anti_noise"]);
    if (j.contains("camera_ids")) r.camera_ids = j["camera_ids"].get<std::vector<int>>();
    if (j.contains("metadata")) r.metadata = j["metadata"];
    if (j.contains("created_at")) r.created_at = std::chrono::system_clock::from_time_t(j["created_at"]);
    if (j.contains("updated_at")) r.updated_at = std::chrono::system_clock::from_time_t(j["updated_at"]);
    
    return r;
}

// ============================================================================
// SERIALIZATION â€” RuleTriggerLog
// ============================================================================

nlohmann::json RuleTriggerLog::toJSON() const {
    return {
        {"log_id", log_id},
        {"rule_id", rule_id},
        {"rule_name", rule_name},
        {"event_id", event_id},
        {"camera_id", camera_id},
        {"event_type", eventTypeToString(event_type)},
        {"actions_taken", actions_taken},
        {"suppressed", suppressed},
        {"suppression_reason", suppression_reason},
        {"triggered_at", std::chrono::system_clock::to_time_t(triggered_at)}
    };
}

// ============================================================================
// RULE ENGINE â€” Singleton
// ============================================================================

RuleEngine& RuleEngine::getInstance() {
    static RuleEngine instance;
    return instance;
}

// ============================================================================
// RULE CRUD
// ============================================================================

int RuleEngine::addRule(const CompositeRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rule.rule_id != 0) {
        for (auto& existing : rules_) {
            if (existing.rule_id == rule.rule_id) {
                LOG_WARN("[RuleEngine] Rule {} already exists", rule.rule_id);
                return 0;
            }
        }
    }
    CompositeRule r = rule;
    if (r.rule_id == 0) r.rule_id = next_rule_id_++;
    r.created_at = std::chrono::system_clock::now();
    r.updated_at = r.created_at;
    rules_.push_back(r);
    if (r.rule_id >= next_rule_id_) next_rule_id_ = r.rule_id + 1;
    LOG_INFO("[RuleEngine] Added rule {}: {}", r.rule_id, r.name);
    return r.rule_id;
}

bool RuleEngine::updateRule(const CompositeRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : rules_) {
        if (r.rule_id == rule.rule_id) {
            r = rule;
            r.updated_at = std::chrono::system_clock::now();
            LOG_INFO("[RuleEngine] Updated rule {}: {}", r.rule_id, r.name);
            return true;
        }
    }
    return false;
}

bool RuleEngine::removeRule(int rule_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(rules_.begin(), rules_.end(),
        [rule_id](const CompositeRule& r) { return r.rule_id == rule_id; });
    if (it == rules_.end()) return false;
    rules_.erase(it);
    noise_state_.erase(rule_id);
    rate_window_.erase(rule_id);
    LOG_INFO("[RuleEngine] Removed rule {}", rule_id);
    return true;
}

std::optional<CompositeRule> RuleEngine::getRule(int rule_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : rules_) {
        if (r.rule_id == rule_id) return r; // copy out — caller may use after lock
    }
    return std::nullopt;
}

std::vector<CompositeRule> RuleEngine::getAllRules() {
    std::lock_guard<std::mutex> lock(mutex_);
    return rules_;
}

int RuleEngine::getNextRuleId() {
    // Reserve and return — without post-increment two concurrent UI clients
    // would receive the same id, then both addRule() calls would race to insert
    // the same rule_id (one losing with "already exists" log, silently dropped).
    std::lock_guard<std::mutex> lock(mutex_);
    return next_rule_id_++;
}

// ============================================================================
// CORE EVALUATION
// ============================================================================

bool RuleEngine::evaluateEvent(const RawEvent& event) {
    // ARCH-004 FIX: Collect actions to execute outside the lock
    struct PendingAction {
        CompositeRule rule;
        RawEvent event;
    };
    std::vector<PendingAction> pending_actions;
    std::vector<RuleFiredCallback> local_callbacks;
    
    // BUG-3 followup (2026-05-22): snapshot trigger_log_ once at top of
    // evaluation so RULE_TRIGGERED conditions read from this local copy
    // without acquiring log_mutex_ during evaluateCondition. Pre-fix used
    // try_lock and silently dropped RULE_TRIGGERED checks on contention
    // (chained-rule cascade evaluation lost under load). Brief copy here
    // is bounded by trigger_log_.size() ≤ 10000 (FIFO cap) and runs
    // outside any other mutex, so contention window is small. The
    // snapshot is slightly stale (won't see firings recorded in this
    // very evaluation), which is acceptable — chained rules already
    // operate on prior firings by definition.
    std::vector<RuleTriggerLog> trigger_log_snapshot;
    {
        std::lock_guard<std::mutex> llock(log_mutex_);
        trigger_log_snapshot = trigger_log_;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        total_evaluated_++;
        local_callbacks = callbacks_;

        for (auto& rule : rules_) {
            if (!rule.enabled) continue;

            // Check camera scope
            if (!rule.camera_ids.empty()) {
                if (std::find(rule.camera_ids.begin(), rule.camera_ids.end(),
                              event.camera_id) == rule.camera_ids.end()) {
                    continue;
                }
            }

            // Evaluate conditions
            if (!evaluateConditions(rule.conditions, rule.logic, event, trigger_log_snapshot)) {
                continue;
            }
            
            // Anti-noise check
            if (!checkAntiNoise(rule, event)) {
                // Log suppression
                RuleTriggerLog tlog;
                tlog.rule_id = rule.rule_id;
                tlog.rule_name = rule.name;
                tlog.event_id = event.event_id;
                tlog.camera_id = event.camera_id;
                tlog.event_type = event.type;
                tlog.suppressed = true;
                tlog.triggered_at = std::chrono::system_clock::now();
                {
                    std::lock_guard<std::mutex> llock(log_mutex_);
                    trigger_log_.push_back(tlog);
                    if (trigger_log_.size() > 10000) trigger_log_.erase(trigger_log_.begin());
                }
                total_suppressed_++;
                continue;
            }
            
            // Rule matched! Record trigger
            auto now = std::chrono::system_clock::now();
            noise_state_[rule.rule_id].last_trigger = now;
            noise_state_[rule.rule_id].consecutive_count++;
            // Record for max_alerts_per_hour rolling window (checkRateLimit
            // reads this on the next evaluation; pruning happens lazily there).
            rate_window_[rule.rule_id].push_back(now);
            
            RuleTriggerLog tlog;
            tlog.log_id = total_triggered_ + 1;
            tlog.rule_id = rule.rule_id;
            tlog.rule_name = rule.name;
            tlog.event_id = event.event_id;
            tlog.camera_id = event.camera_id;
            tlog.event_type = event.type;
            tlog.suppressed = false;
            tlog.triggered_at = now;
            
            // Build actions summary
            nlohmann::json actions_json = nlohmann::json::array();
            for (auto& a : rule.actions) actions_json.push_back(actionTypeToString(a.type));
            tlog.actions_taken = actions_json.dump();
            
            {
                std::lock_guard<std::mutex> llock(log_mutex_);
                trigger_log_.push_back(tlog);
                if (trigger_log_.size() > 10000) trigger_log_.erase(trigger_log_.begin());
            }
            
            total_triggered_++;
            
            LOG_INFO("[RuleEngine] Rule {} '{}' triggered by event {} on cam {}",
                     rule.rule_id, rule.name, eventTypeToString(event.type), event.camera_id);
            
            // ARCH-004 FIX: Defer action execution to outside the lock
            pending_actions.push_back({rule, event});
        }
    } // mutex_ released here
    
    // Execute actions and callbacks OUTSIDE the lock to prevent deadlocks
    bool any_triggered = !pending_actions.empty();
    for (auto& pa : pending_actions) {
        executeActions(pa.rule, pa.event);

        // BUG-2 FIX: dùng local_callbacks (đã copy trong lock) thay vì callbacks_
        // để tránh race condition khi onRuleFired() được gọi từ thread khác
        for (auto& cb : local_callbacks) {
            try { cb(pa.rule, pa.event); } catch (...) {}
        }
    }

    return any_triggered;
}

// ============================================================================
// CONDITION EVALUATION
// ============================================================================

bool RuleEngine::evaluateConditions(const std::vector<RuleCondition>& conds,
                                     RuleLogic logic, const RawEvent& event,
                                     const std::vector<RuleTriggerLog>& trigger_log_snapshot) {
    if (conds.empty()) return true;

    if (logic == RuleLogic::AND) {
        for (auto& c : conds) {
            if (!evaluateCondition(c, event, trigger_log_snapshot)) return false;
        }
        return true;
    } else {
        // OR
        for (auto& c : conds) {
            if (evaluateCondition(c, event, trigger_log_snapshot)) return true;
        }
        return false;
    }
}

bool RuleEngine::evaluateCondition(const RuleCondition& cond, const RawEvent& event,
                                   const std::vector<RuleTriggerLog>& trigger_log_snapshot) {
    switch (cond.type) {
        case ConditionType::OBJECT:
            return cond.object_class.empty() || event.object_class == cond.object_class;
            
        case ConditionType::ZONE: {
            if (cond.zone_id < 0) return true;
            if (cond.zone_action == "enter" || cond.zone_action == "stay") {
                return event.zone_id == cond.zone_id;
            }
            // For "exit" we'd need tracking state â€” simplified: check zone_id
            return event.zone_id == cond.zone_id;
        }
        
        case ConditionType::TIME: {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            auto tm = std::localtime(&t);
            int hour = tm->tm_hour;
            int day = tm->tm_wday;
            
            // Handle overnight ranges (e.g., 22:00 - 06:00)
            bool in_range;
            if (cond.start_hour <= cond.end_hour) {
                in_range = (hour >= cond.start_hour && hour <= cond.end_hour);
            } else {
                in_range = (hour >= cond.start_hour || hour <= cond.end_hour);
            }
            if (!in_range) return false;
            
            if (!cond.days_of_week.empty()) {
                if (std::find(cond.days_of_week.begin(), cond.days_of_week.end(),
                              day) == cond.days_of_week.end()) {
                    return false;
                }
            }
            return true;
        }
        
        case ConditionType::BEHAVIOR:
            return event.type == cond.behavior_type;
            
        case ConditionType::EVENT_TYPE:
            if (cond.event_types.empty()) return true;
            return std::find(cond.event_types.begin(), cond.event_types.end(),
                             event.type) != cond.event_types.end();
            
        case ConditionType::SEVERITY:
            return event.severity >= cond.min_severity;
            
        case ConditionType::CONFIDENCE:
            return event.confidence >= cond.min_confidence;
            
        case ConditionType::CAMERA:
            if (cond.camera_ids.empty()) return true;
            return std::find(cond.camera_ids.begin(), cond.camera_ids.end(),
                             event.camera_id) != cond.camera_ids.end();
            
        case ConditionType::RULE_TRIGGERED: {
            if (cond.referenced_rule_id < 0) return true;

            // BUG-3 followup (2026-05-22): read from the snapshot
            // evaluateEvent already collected under log_mutex_. No
            // lock acquired here → no deadlock risk and no
            // try_lock-induced silent drops of chained-rule
            // evaluations under contention.
            auto now = std::chrono::system_clock::now();
            for (auto it = trigger_log_snapshot.rbegin(); it != trigger_log_snapshot.rend(); ++it) {
                if (it->rule_id == cond.referenced_rule_id) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - it->triggered_at).count();

                    if (elapsed <= cond.chaining_window_sec) {
                        // Found a recent trigger!
                        if (it->camera_id == event.camera_id) return true;
                    } else {
                        // Beyond window, and logs are chronological, so we can stop
                        break;
                    }
                }
            }
            return false;
        }
            
        default:
            return true;
    }
}

// ============================================================================
// ANTI-NOISE
// ============================================================================

bool RuleEngine::checkAntiNoise(const CompositeRule& rule, const RawEvent& event) {
    auto& an = rule.anti_noise;
    
    // Confidence threshold
    if (event.confidence < an.confidence_threshold) {
        return false;
    }
    
    // Cooldown check
    if (!checkCooldown(rule.rule_id, an.cooldown_sec)) {
        return false;
    }
    
    // Debounce check (must see event for N seconds before triggering)
    if (an.debounce_sec > 0) {
        if (!checkDebounce(rule.rule_id, event.camera_id, an.debounce_sec)) {
            return false;
        }
    }

    // Rate limit (max_alerts_per_hour). 0 / negative ⇒ unlimited.
    if (an.max_alerts_per_hour > 0) {
        if (!checkRateLimit(rule.rule_id, an.max_alerts_per_hour)) {
            return false;
        }
    }

    return true;
}

bool RuleEngine::checkRateLimit(int rule_id, int max_per_hour) {
    auto& window = rate_window_[rule_id];
    const auto now = std::chrono::system_clock::now();
    const auto one_hour_ago = now - std::chrono::hours(1);
    // Prune; std::vector erase-from-front is O(n) but n is bounded by
    // max_per_hour, so even at 1000 alerts/hr this is sub-microsecond and
    // runs at most once per evaluated event (already under mutex_).
    window.erase(std::remove_if(window.begin(), window.end(),
                                [one_hour_ago](const auto& t) { return t < one_hour_ago; }),
                 window.end());
    return window.size() < static_cast<size_t>(max_per_hour);
}

bool RuleEngine::checkCooldown(int rule_id, int cooldown_sec) {
    auto it = noise_state_.find(rule_id);
    if (it == noise_state_.end()) return true;
    
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - it->second.last_trigger).count();
    
    return elapsed >= cooldown_sec;
}

bool RuleEngine::checkDebounce(int rule_id, int camera_id, int debounce_sec) {
    // Debounce semantics: only trigger after the SAME event has been seen
    // continuously for `debounce_sec` seconds. A gap longer than `debounce_sec`
    // resets the streak — otherwise a single isolated event one minute later
    // would immediately fire (the previous bug).
    std::string key = std::to_string(rule_id) + ":" + std::to_string(camera_id);
    auto now = std::chrono::system_clock::now();

    auto it = debounce_state_.find(key);
    if (it == debounce_state_.end()) {
        auto& s = debounce_state_[key];
        s.first_seen = now;
        s.last_seen  = now;
        s.consecutive_count = 1;
        return false;
    }

    auto& state = it->second;
    auto gap_sec = std::chrono::duration_cast<std::chrono::seconds>(
        now - state.last_seen).count();
    if (gap_sec > debounce_sec) {
        // Stream of events broke — restart the debounce window from this event.
        state.first_seen = now;
        state.last_seen  = now;
        state.consecutive_count = 1;
        return false;
    }

    state.last_seen = now;
    auto streak_sec = std::chrono::duration_cast<std::chrono::seconds>(
        now - state.first_seen).count();
    if (streak_sec >= debounce_sec) {
        // Sustained signal — fire and reset so the next trigger needs another full window.
        state.first_seen = now;
        state.consecutive_count = 0;
        return true;
    }

    state.consecutive_count++;
    return false;
}

bool RuleEngine::checkMinDuration(const RawEvent& event, int min_duration_sec) {
    // Would need tracking state to know how long an object has been visible
    // For now, always pass (to be integrated with tracker later)
    (void)event;
    (void)min_duration_sec;
    return true;
}

// ============================================================================
// ACTION EXECUTION
// ============================================================================

void RuleEngine::executeActions(const CompositeRule& rule, const RawEvent& event) {
    for (auto& action : rule.actions) {
        switch (action.type) {
            case ActionType::ALERT: {
                // 2026-05-14 consolidation: pre-fix this called
                // AlertRouter::routeEvent(CorrelatedEvent) which then matched
                // against AlertRouter's in-memory rule list — that list was
                // NEVER populated (addRule had zero callers), so every
                // ALERT action silently dropped. Now delivers directly via
                // the unified alert_delivery layer, reading per-channel
                // recipients from action.metadata.
                deliverAction(rule, action, event);
                break;
            }
            case ActionType::RECORD_CLIP: {
                LOG_INFO("[RuleEngine] Action: RECORD_CLIP pre={}s post={}s for cam {}",
                         action.pre_event_sec, action.post_event_sec, event.camera_id);
                try {
                    auto& pipeline_mgr = vms::core::CameraPipelineManager::getInstance();
                    std::string clip_event_id = "rule_" + std::to_string(rule.rule_id) + "_" + 
                                               std::to_string(std::time(nullptr));
                    int post_sec = action.post_event_sec > 0 ? action.post_event_sec : 20;
                    int pre_sec = action.pre_event_sec > 0 ? action.pre_event_sec : 10;
                    std::string video_path = pipeline_mgr.triggerEventRecording(
                        event.camera_id, clip_event_id, post_sec, pre_sec);
                    if (!video_path.empty()) {
                        LOG_INFO("[RuleEngine] Recording triggered: {}", video_path);
                    } else {
                        LOG_WARN("[RuleEngine] RECORD_CLIP failed for cam {}", event.camera_id);
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("[RuleEngine] RECORD_CLIP exception: {}", e.what());
                }
                break;
            }
            case ActionType::SNAPSHOT: {
                LOG_INFO("[RuleEngine] Action: SNAPSHOT for cam {}", event.camera_id);
                try {
                    auto& snap_mgr = vms::core::SnapshotManager::getInstance();
                    std::string path = snap_mgr.takeSnapshot(event.camera_id);
                    if (!path.empty()) {
                        LOG_INFO("[RuleEngine] Snapshot saved: {}", path);
                    } else {
                        LOG_WARN("[RuleEngine] SNAPSHOT failed for cam {}", event.camera_id);
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("[RuleEngine] SNAPSHOT exception: {}", e.what());
                }
                break;
            }
            case ActionType::WEBHOOK: {
                // 2026-05-14 consolidation: identical no-op problem as ALERT
                // pre-fix. Now goes through alert_delivery::deliverAction,
                // which reads action.webhook_url and applies SSRF + DNS-pin
                // before the async POST.
                deliverAction(rule, action, event);
                break;
            }
            case ActionType::LOG:
                LOG_INFO("[RuleEngine] Action: LOG event {} rule {}", 
                         eventTypeToString(event.type), rule.name);
                break;
        }
    }
}

// ============================================================================
// TRIGGER LOG
// ============================================================================

std::vector<RuleTriggerLog> RuleEngine::getRecentTriggers(int limit) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    int start = std::max(0, (int)trigger_log_.size() - limit);
    return std::vector<RuleTriggerLog>(
        trigger_log_.begin() + start, trigger_log_.end());
}

// ============================================================================
// PERSISTENCE (Qt SQL)
// ============================================================================

bool RuleEngine::loadFromDatabase() {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_INFO("[RuleEngine] Loading rules from database...");
    
    auto& db_mgr = database::DbManager::getInstance();
    if (!db_mgr.isInitialized()) return false;
    
    QSqlDatabase db = db_mgr.getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_ERROR("[RuleEngine] Failed to get database connection");
        return false;
    }
    
    QSqlQuery query(db);
    if (!query.exec("SELECT id, name, description, enabled, logic, conditions_json, "
                    "actions_json, anti_noise_json, camera_ids_json, metadata_json, "
                    "created_at, updated_at FROM rules")) {
        LOG_ERROR("[RuleEngine] Failed to load rules: {}", query.lastError().text().toStdString());
        return false;
    }
    
    rules_.clear();
    int max_id = 0;
    
    while (query.next()) {
        CompositeRule r;
        r.rule_id = query.value(0).toInt();
        if (r.rule_id > max_id) max_id = r.rule_id;
        
        r.name = query.value(1).toString().toStdString();
        
        QVariant desc_val = query.value(2);
        if (!desc_val.isNull()) r.description = desc_val.toString().toStdString();
        
        r.enabled = query.value(3).toInt() != 0;
        
        std::string logic_str = query.value(4).toString().toStdString();
        r.logic = (logic_str == "OR") ? RuleLogic::OR : RuleLogic::AND;
        
        // Parse conditions JSON
        QVariant conds_val = query.value(5);
        if (!conds_val.isNull()) {
            try {
                auto conds_j = nlohmann::json::parse(conds_val.toString().toStdString());
                for (auto& c : conds_j) r.conditions.push_back(RuleCondition::fromJSON(c));
            } catch(...) {}
        }
        
        // Parse actions JSON
        QVariant acts_val = query.value(6);
        if (!acts_val.isNull()) {
            try {
                auto acts_j = nlohmann::json::parse(acts_val.toString().toStdString());
                for (auto& a : acts_j) r.actions.push_back(RuleAction::fromJSON(a));
            } catch(...) {}
        }
        
        // Parse anti_noise JSON
        QVariant noise_val = query.value(7);
        if (!noise_val.isNull()) {
            try {
                auto noise_j = nlohmann::json::parse(noise_val.toString().toStdString());
                r.anti_noise = AntiNoiseConfig::fromJSON(noise_j);
            } catch(...) {}
        }
        
        // Parse camera_ids JSON
        QVariant cams_val = query.value(8);
        if (!cams_val.isNull()) {
            try {
                auto cams_j = nlohmann::json::parse(cams_val.toString().toStdString());
                r.camera_ids = cams_j.get<std::vector<int>>();
            } catch(...) {}
        }
        
        // Parse metadata JSON
        QVariant meta_val = query.value(9);
        if (!meta_val.isNull()) {
            try { r.metadata = nlohmann::json::parse(meta_val.toString().toStdString()); } catch(...) {}
        }
        
        r.created_at = std::chrono::system_clock::from_time_t(query.value(10).toLongLong());
        r.updated_at = std::chrono::system_clock::from_time_t(query.value(11).toLongLong());
        
        rules_.push_back(r);
    }
    
    next_rule_id_ = max_id + 1;
    LOG_INFO("[RuleEngine] Loaded {} rules", rules_.size());
    return true;
}

bool RuleEngine::saveToDatabase() {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_INFO("[RuleEngine] Saving rules to database...");
    
    auto& db_mgr = database::DbManager::getInstance();
    if (!db_mgr.isInitialized()) return false;
    
    QSqlDatabase db = db_mgr.getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_ERROR("[RuleEngine] Failed to get database connection");
        return false;
    }
    
    db_mgr.beginTransaction();
    
    // Prepare upsert statement — dialect-aware
    const QString upsert_sql = db_mgr.isPostgres()
        ? "INSERT INTO rules (id, name, description, enabled, logic, "
          "conditions_json, actions_json, anti_noise_json, camera_ids_json, "
          "metadata_json, created_at, updated_at) "
          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
          "ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, description=EXCLUDED.description, "
          "enabled=EXCLUDED.enabled, logic=EXCLUDED.logic, conditions_json=EXCLUDED.conditions_json, "
          "actions_json=EXCLUDED.actions_json, anti_noise_json=EXCLUDED.anti_noise_json, "
          "camera_ids_json=EXCLUDED.camera_ids_json, metadata_json=EXCLUDED.metadata_json, "
          "updated_at=EXCLUDED.updated_at"
        : "INSERT OR REPLACE INTO rules (id, name, description, enabled, logic, "
          "conditions_json, actions_json, anti_noise_json, camera_ids_json, "
          "metadata_json, created_at, updated_at) "
          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    QSqlQuery query(db);
    if (!query.prepare(upsert_sql)) {
        LOG_ERROR("[RuleEngine] Failed to prepare save query: {}", query.lastError().text().toStdString());
        db_mgr.rollback();
        return false;
    }
    
    for (const auto& r : rules_) {
        query.bindValue(0, r.rule_id);
        query.bindValue(1, QString::fromStdString(r.name));
        query.bindValue(2, QString::fromStdString(r.description));
        query.bindValue(3, r.enabled ? 1 : 0);
        query.bindValue(4, QString::fromLatin1((r.logic == RuleLogic::AND) ? "AND" : "OR"));
        
        nlohmann::json conds_j = nlohmann::json::array();
        for (auto& c : r.conditions) conds_j.push_back(c.toJSON());
        query.bindValue(5, QString::fromStdString(conds_j.dump()));
        
        nlohmann::json acts_j = nlohmann::json::array();
        for (auto& a : r.actions) acts_j.push_back(a.toJSON());
        query.bindValue(6, QString::fromStdString(acts_j.dump()));
        
        query.bindValue(7, QString::fromStdString(r.anti_noise.toJSON().dump()));
        
        nlohmann::json cams_j = r.camera_ids;
        query.bindValue(8, QString::fromStdString(cams_j.dump()));
        
        query.bindValue(9, QString::fromStdString(r.metadata.dump()));
        
        query.bindValue(10, static_cast<qint64>(std::chrono::system_clock::to_time_t(r.created_at)));
        query.bindValue(11, static_cast<qint64>(std::chrono::system_clock::to_time_t(r.updated_at)));
        
        if (!query.exec()) {
            LOG_ERROR("[RuleEngine] Failed to insert rule {}: {}", r.rule_id, query.lastError().text().toStdString());
        }
    }
    
    // ARCH-005 FIX: Clean up any rules in the database that were removed from memory
    {
        QString active_ids = "0"; // safe default
        for (const auto& r : rules_) {
            active_ids += "," + QString::number(r.rule_id);
        }
        QSqlQuery del_query(db);
        if (!del_query.exec("DELETE FROM rules WHERE id NOT IN (" + active_ids + ")")) {
            LOG_WARN("[RuleEngine] Failed to clean up old rules: {}", del_query.lastError().text().toStdString());
        }
    }

    db_mgr.commit();
    return true;
}

// ============================================================================
// STATISTICS
// ============================================================================

nlohmann::json RuleEngine::getStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"total_rules", rules_.size()},
        {"enabled_rules", std::count_if(rules_.begin(), rules_.end(),
            [](const CompositeRule& r) { return r.enabled; })},
        {"total_evaluated", total_evaluated_.load()},
        {"total_triggered", total_triggered_.load()},
        {"total_suppressed", total_suppressed_.load()},
        {"trigger_log_size", trigger_log_.size()}
    };
}

// ============================================================================
// CALLBACKS
// ============================================================================

void RuleEngine::onRuleFired(RuleFiredCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(cb);
}

// ============================================================================
// LEGACY MIGRATION — `alert_rules` table → CompositeRule
// One-shot, idempotent via the `alert_rules_migrated` setting. Called once
// at boot in main.cpp after loadFromDatabase. The legacy AlertManager that
// used to read this table is gone (2026-05-14 consolidation); without
// migration, any rules an operator created via the legacy POST
// /api/alerts/rules would silently stop delivering.
// ============================================================================

bool RuleEngine::migrateLegacyAlertRules() {
    auto& db_mgr = database::DbManager::getInstance();
    if (!db_mgr.isInitialized()) return false;

    if (db_mgr.getSetting("alert_rules_migrated", "0") == "1") {
        LOG_INFO("[RuleEngine] Legacy alert_rules migration already done; skipping.");
        return true;
    }

    QSqlDatabase db = db_mgr.getThreadConnection();
    if (!db.isValid() || !db.isOpen()) {
        LOG_WARN("[RuleEngine] Cannot migrate legacy alert_rules: no DB connection");
        return false;
    }

    QSqlQuery query(db);
    // Select even disabled rows so the migration preserves operator intent
    // (they may toggle them back on after upgrading).
    if (!query.exec("SELECT id, camera_id, event_type, severity, action_type, recipient, is_enabled "
                    "FROM alert_rules")) {
        // alert_rules table might not exist on a brand-new install; that's fine.
        LOG_INFO("[RuleEngine] Legacy alert_rules table not readable ({}); marking migrated.",
                 query.lastError().text().toStdString());
        db_mgr.setSetting("alert_rules_migrated", "1");
        return true;
    }

    int migrated_count = 0;
    int skipped_count = 0;

    while (query.next()) {
        int legacy_id     = query.value(0).toInt();
        int camera_id     = query.value(1).toInt();
        std::string etype = query.value(2).toString().toStdString();
        std::string sev   = query.value(3).toString().toStdString();
        std::string atype = query.value(4).toString().toStdString();
        std::string recip = query.value(5).toString().toStdString();
        bool enabled      = query.value(6).toInt() != 0;

        if (recip.empty()) {
            LOG_WARN("[RuleEngine] Migration: skipping legacy alert_rule id={} (empty recipient)", legacy_id);
            ++skipped_count;
            continue;
        }

        CompositeRule r;
        r.name = "Migrated: " + (etype.empty() ? std::string("*") : etype) +
                 "@cam" + std::to_string(camera_id);
        r.description = "Auto-migrated from alert_rules row #" + std::to_string(legacy_id) +
                        " on 2026-05-14 consolidation.";
        r.enabled = enabled;
        r.logic = RuleLogic::AND;

        // Camera scope: legacy "-1" = any camera → empty vector (matches all).
        if (camera_id > 0) r.camera_ids = { camera_id };

        // Event type condition unless legacy row was wildcard.
        if (!etype.empty() && etype != "*") {
            RuleCondition cond;
            cond.type = ConditionType::EVENT_TYPE;
            cond.event_types = { stringToEventType(etype) };
            r.conditions.push_back(cond);
        }

        // Severity condition (legacy default 'high'). Only emit if non-trivial.
        if (!sev.empty() && sev != "low") {
            RuleCondition sev_cond;
            sev_cond.type = ConditionType::SEVERITY;
            // Legacy stored lowercase string; modern fromString expects uppercase.
            std::string sev_upper = sev;
            std::transform(sev_upper.begin(), sev_upper.end(), sev_upper.begin(), ::toupper);
            sev_cond.min_severity = stringToEventSeverity(sev_upper);
            r.conditions.push_back(sev_cond);
        }

        RuleAction action;
        if (atype == "webhook") {
            action.type = ActionType::WEBHOOK;
            action.webhook_url = recip;
        } else if (atype == "email") {
            action.type = ActionType::ALERT;
            action.channels = { AlertChannel::EMAIL };
            action.metadata["email_addresses"] = nlohmann::json::array({ recip });
        } else if (atype == "sms") {
            action.type = ActionType::ALERT;
            action.channels = { AlertChannel::SMS };
            action.metadata["phone_numbers"] = nlohmann::json::array({ recip });
        } else {
            LOG_WARN("[RuleEngine] Migration: skipping legacy alert_rule id={} (unknown action_type='{}')",
                     legacy_id, atype);
            ++skipped_count;
            continue;
        }
        r.actions.push_back(action);

        // Preserve audit trail in metadata.
        r.metadata["migrated_from_alert_rule_id"] = legacy_id;

        // addRule + saveToDatabase. addRule locks mutex_ internally; we call
        // it outside any other lock to avoid ordering issues.
        int assigned = addRule(r);
        if (assigned > 0) {
            ++migrated_count;
            LOG_INFO("[RuleEngine] Migrated legacy alert_rule #{} → composite rule #{}",
                     legacy_id, assigned);
        } else {
            LOG_WARN("[RuleEngine] Migration failed to addRule for legacy id={}", legacy_id);
            ++skipped_count;
        }
    }

    if (migrated_count > 0) {
        if (!saveToDatabase()) {
            LOG_ERROR("[RuleEngine] Migration: addRule succeeded for {} rules but saveToDatabase failed. "
                      "NOT marking migrated — will retry on next boot.", migrated_count);
            return false;
        }
    }

    db_mgr.setSetting("alert_rules_migrated", "1");
    LOG_INFO("[RuleEngine] Legacy alert_rules migration complete: {} migrated, {} skipped.",
             migrated_count, skipped_count);
    return true;
}

} // namespace vms::events

