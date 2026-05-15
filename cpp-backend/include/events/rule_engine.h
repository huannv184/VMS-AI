// ==============================================================
// File: include/events/rule_engine.h
// Composite Rule Engine with Anti-Noise
// ==============================================================

#pragma once

#include "events/event_types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <functional>
#include <atomic>
#include <optional>
#include <nlohmann/json.hpp>

namespace vms::events {

// ============================================================================
// ALERT CHANNELS — recipients per RuleAction
// Moved here from the deleted events/alert_router.h on 2026-05-14
// (AlertManager/AlertRouter consolidation). RuleAction.channels is the
// canonical place for these.
// ============================================================================

enum class AlertChannel {
    NONE,
    UI_NOTIFICATION,    // Web UI popup (WS broadcast)
    EMAIL,              // SMTP via vms::utils::sendEmailAsync
    SMS,                // Twilio HTTPS
    WEBHOOK,            // HTTP POST to external system
    MOBILE_PUSH,        // Telegram bot
    ALARM_OUTPUT        // LAN relay HTTP POST
};

const char* alertChannelToString(AlertChannel channel);
AlertChannel stringToAlertChannel(const std::string& str);

// ============================================================================
// RULE CONDITION — Single condition in a composite rule
// ============================================================================

enum class ConditionType {
    OBJECT,         // Object class filter (person, car, etc.)
    ZONE,           // In/out of zone
    TIME,           // Time window
    BEHAVIOR,       // Behavior type (loitering, running, etc.)
    EVENT_TYPE,     // Specific event type
    SEVERITY,       // Min severity level
    CONFIDENCE,     // Min confidence threshold
    CAMERA,         // Specific camera(s)
    RULE_TRIGGERED  // Sequential rule chaining
};

struct RuleCondition {
    ConditionType type;
    
    // OBJECT condition
    std::string object_class;           // "person", "car", "truck"
    
    // ZONE condition
    int zone_id{-1};
    std::string zone_action{"enter"};   // "enter", "exit", "stay", "cross"
    
    // TIME condition
    int start_hour{0};
    int end_hour{23};
    std::vector<int> days_of_week;      // 0=Sun, 6=Sat. Empty = all days
    
    // BEHAVIOR condition
    EventType behavior_type{EventType::UNKNOWN};
    int duration_sec{0};                // Min duration for loitering, etc.
    
    // EVENT_TYPE condition
    std::vector<EventType> event_types;
    
    // SEVERITY condition
    EventSeverity min_severity{EventSeverity::LOW};
    
    // CONFIDENCE condition
    float min_confidence{0.0f};
    
    // CAMERA condition
    std::vector<int> camera_ids;
    
    // RULE_TRIGGERED condition (Chaining)
    int referenced_rule_id{-1};
    int chaining_window_sec{30};
    
    // Serialization
    nlohmann::json toJSON() const;
    static RuleCondition fromJSON(const nlohmann::json& j);
};

// ============================================================================
// RULE ACTION — What to do when rule triggers
// ============================================================================

enum class ActionType {
    ALERT,          // Send alert via channels
    RECORD_CLIP,    // Record pre/post event clip
    SNAPSHOT,       // Take JPEG snapshot
    WEBHOOK,        // Call external URL
    LOG             // Log to database
};

struct RuleAction {
    ActionType type;
    
    // ALERT action
    std::vector<AlertChannel> channels;
    
    // RECORD_CLIP action
    int pre_event_sec{10};
    int post_event_sec{30};
    
    // WEBHOOK action
    std::string webhook_url;
    
    // Metadata
    nlohmann::json metadata;
    
    nlohmann::json toJSON() const;
    static RuleAction fromJSON(const nlohmann::json& j);
};

// ============================================================================
// ANTI-NOISE CONFIG — Prevent alert spam
// ============================================================================

struct AntiNoiseConfig {
    int cooldown_sec{60};           // Min time between alerts for same rule
    int debounce_sec{3};            // Event must persist X seconds to trigger
    int min_duration_sec{2};        // Object must be visible X seconds
    float confidence_threshold{0.5f}; // Min confidence to consider
    int max_alerts_per_hour{60};    // Rate limit
    
    nlohmann::json toJSON() const;
    static AntiNoiseConfig fromJSON(const nlohmann::json& j);
};

// ============================================================================
// COMPOSITE RULE — Full rule definition
// ============================================================================

enum class RuleLogic {
    AND,    // All conditions must match
    OR      // Any condition must match
};

struct CompositeRule {
    int rule_id{0};
    std::string name;
    std::string description;
    bool enabled{true};
    
    // Conditions
    RuleLogic logic{RuleLogic::AND};
    std::vector<RuleCondition> conditions;
    
    // Actions
    std::vector<RuleAction> actions;
    
    // Anti-noise
    AntiNoiseConfig anti_noise;
    
    // Camera scope (empty = all cameras)
    std::vector<int> camera_ids;
    
    // Metadata
    nlohmann::json metadata;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    
    nlohmann::json toJSON() const;
    static CompositeRule fromJSON(const nlohmann::json& j);
};

// ============================================================================
// RULE TRIGGER LOG — Record when a rule fires
// ============================================================================

struct RuleTriggerLog {
    uint64_t log_id{0};
    int rule_id{0};
    std::string rule_name;
    uint64_t event_id{0};
    int camera_id{0};
    EventType event_type{EventType::UNKNOWN};
    std::string actions_taken;      // JSON string of actions executed
    bool suppressed{false};         // Was it suppressed by anti-noise?
    std::string suppression_reason; // "cooldown", "debounce", "confidence"
    std::chrono::system_clock::time_point triggered_at;
    
    nlohmann::json toJSON() const;
};

// ============================================================================
// RULE ENGINE (Singleton)
// ============================================================================

class RuleEngine {
public:
    static RuleEngine& getInstance();
    
    // Rule CRUD
    // BUG-H5 FIX: addRule returns the assigned rule_id (0 on failure) so
    // callers don't have to look at the input copy — the copy is what we
    // mutated, the caller's original was const&. Returning the id keeps the
    // POST handler honest about what id the client was actually given.
    int addRule(const CompositeRule& rule);
    bool updateRule(const CompositeRule& rule);
    bool removeRule(int rule_id);
    // BUG-H4 FIX: was returning a raw pointer into the internal vector after
    // releasing the lock — any concurrent addRule() push_back could invalidate
    // it (UAF). Returning by value is the safe equivalent for read-only use.
    std::optional<CompositeRule> getRule(int rule_id);
    std::vector<CompositeRule> getAllRules();
    int getNextRuleId();
    
    // Core evaluation
    bool evaluateEvent(const RawEvent& event);
    
    // Trigger log
    std::vector<RuleTriggerLog> getRecentTriggers(int limit = 100);
    
    // Persistence
    bool loadFromDatabase();
    bool saveToDatabase();

    // One-shot migration from the legacy `alert_rules` table into composite
    // `rules`. Gated by the `alert_rules_migrated` setting so it's safe to
    // call on every boot. Each legacy row becomes one CompositeRule with a
    // single EVENT_TYPE condition (or none if event_type='*') and one
    // ALERT/WEBHOOK action carrying the recipient in metadata. After
    // migration the legacy rows are left in place — operators can DROP
    // TABLE alert_rules manually if desired (the table is no longer read
    // by anything after this pass). Returns true if a migration ran (or
    // was already done previously); false on hard DB error.
    bool migrateLegacyAlertRules();
    
    // Statistics
    nlohmann::json getStatistics();
    
    // Callback for when a rule fires (after anti-noise)
    using RuleFiredCallback = std::function<void(const CompositeRule&, const RawEvent&)>;
    void onRuleFired(RuleFiredCallback cb);
    
private:
    RuleEngine() = default;
    ~RuleEngine() = default;
    RuleEngine(const RuleEngine&) = delete;
    RuleEngine& operator=(const RuleEngine&) = delete;
    
    // Evaluation internals
    bool evaluateCondition(const RuleCondition& cond, const RawEvent& event);
    bool evaluateConditions(const std::vector<RuleCondition>& conds, RuleLogic logic, const RawEvent& event);
    void executeActions(const CompositeRule& rule, const RawEvent& event);
    
    // Anti-noise checks
    bool checkAntiNoise(const CompositeRule& rule, const RawEvent& event);
    bool checkCooldown(int rule_id, int cooldown_sec);
    bool checkDebounce(int rule_id, int camera_id, int debounce_sec);
    bool checkMinDuration(const RawEvent& event, int min_duration_sec);
    // Returns true if the rule has fewer than max_per_hour trigger entries
    // recorded in the last hour. Read-only — actual recording happens in
    // evaluateEvent when the rule fires, so per-rule window state survives
    // alongside noise_state_ entry mutations.
    bool checkRateLimit(int rule_id, int max_per_hour);
    
    // Data
    std::vector<CompositeRule> rules_;
    std::vector<RuleTriggerLog> trigger_log_;
    
    // Anti-noise state
    struct NoiseState {
        std::chrono::system_clock::time_point last_trigger;
        std::chrono::system_clock::time_point first_seen;  // For debounce
        std::chrono::system_clock::time_point last_seen;   // For debounce gap detection
        int consecutive_count{0};
    };
    std::unordered_map<int, NoiseState> noise_state_;  // key = rule_id
    std::unordered_map<std::string, NoiseState> debounce_state_;  // key = "rule_id:camera_id"
    // Rolling 1h window of trigger timestamps per rule for max_alerts_per_hour
    // enforcement. Pre-2026-05-14 this field existed on the deleted AlertRouter
    // but RuleEngine itself never enforced anti_noise.max_alerts_per_hour.
    std::unordered_map<int, std::vector<std::chrono::system_clock::time_point>> rate_window_;
    
    // Stats
    std::atomic<uint64_t> total_evaluated_{0};
    std::atomic<uint64_t> total_triggered_{0};
    std::atomic<uint64_t> total_suppressed_{0};
    int next_rule_id_{1};
    
    // Callbacks
    std::vector<RuleFiredCallback> callbacks_;
    
    // Thread safety
    mutable std::mutex mutex_;
    mutable std::mutex log_mutex_;
};

} // namespace vms::events
