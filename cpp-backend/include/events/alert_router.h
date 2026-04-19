// ==============================================================
// File: include/events/alert_router.h
// Alert Routing & Notification System
// ==============================================================

#pragma once

#include "events/event_types.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <QObject>
#include <QThread>

namespace vms::events {

// ============================================================================
// ALERT CHANNELS
// ============================================================================

enum class AlertChannel {
    NONE,
    UI_NOTIFICATION,    // Web UI popup
    EMAIL,              // Email notification
    SMS,                // SMS text message
    WEBHOOK,            // HTTP POST to external system
    MOBILE_PUSH,        // Mobile app push notification
    ALARM_OUTPUT        // Physical alarm relay
};

const char* alertChannelToString(AlertChannel channel);
AlertChannel stringToAlertChannel(const std::string& str);

// ============================================================================
// ALERT RULE
// ============================================================================

struct AlertRule {
    // Identity
    int rule_id{0};
    std::string name;
    bool enabled{true};
    
    // Conditions
    std::vector<EventType> event_types;     // Empty = all types
    std::vector<int> zone_ids;              // Empty = all zones
    EventSeverity min_severity{EventSeverity::MEDIUM};
    
    // Time-based restrictions
    bool time_restricted{false};
    int start_hour{0};                      // 24-hour format
    int end_hour{23};
    std::vector<int> days_of_week;          // 0=Sunday, 6=Saturday
    
    // Routing
    std::vector<AlertChannel> channels;
    
    // Rate limiting
    int max_per_hour{60};
    
    // Recipients
    std::vector<std::string> email_addresses;
    std::vector<std::string> phone_numbers;
    std::string webhook_url;
    
    // Metadata
    nlohmann::json metadata;
    std::chrono::system_clock::time_point created_at;
    
    // Serialization
    nlohmann::json toJSON() const;
    static AlertRule fromJSON(const nlohmann::json& j);
};

// ============================================================================
// ALERT ROUTER (Singleton)
// ============================================================================

class AlertRouter : public QObject {
    Q_OBJECT
public:
    static AlertRouter& getInstance();
    
    // Route correlated event through alert rules
    void routeEvent(const CorrelatedEvent& event);
    
    // Rule management
    bool addRule(const AlertRule& rule);
    bool removeRule(int rule_id);
    bool updateRule(const AlertRule& rule);
    AlertRule* getRule(int rule_id);
    std::vector<AlertRule> getAllRules();
    
    // Channel handlers (register custom handlers)
    using ChannelHandler = std::function<void(const CorrelatedEvent&, const AlertRule&)>;
    void registerChannelHandler(AlertChannel channel, ChannelHandler handler);
    
    // Test alert (for rule testing)
    bool testAlert(int rule_id);
    
    // Statistics
    nlohmann::json getStatistics();

    void shutdown();
    
Q_SIGNALS:
    void eventQueued();

private Q_SLOTS:
    void processQueue();

private:
    AlertRouter();
    ~AlertRouter();
    
    // Prevent copying
    AlertRouter(const AlertRouter&) = delete;
    AlertRouter& operator=(const AlertRouter&) = delete;
    
    // Evaluation
    bool evaluateRule(const AlertRule& rule, const CorrelatedEvent& event);
    bool isWithinTimeWindow(const AlertRule& rule);
    bool withinRateLimit(int rule_id);
    void recordAlert(int rule_id);
    
    // Send to channels
    void sendToChannels(const CorrelatedEvent& event, const AlertRule& rule);
    void sendToChannel(AlertChannel channel, const CorrelatedEvent& event, 
                      const AlertRule& rule);
    
    // Built-in channel handlers
    void sendUINotification(const CorrelatedEvent& event, const AlertRule& rule);
    void sendEmail(const CorrelatedEvent& event, const AlertRule& rule);
    void sendSMS(const CorrelatedEvent& event, const AlertRule& rule);
    void sendWebhook(const CorrelatedEvent& event, const AlertRule& rule);
    void sendMobilePush(const CorrelatedEvent& event, const AlertRule& rule);
    void triggerAlarmOutput(const CorrelatedEvent& event, const AlertRule& rule);
    
    // Data
    std::vector<AlertRule> rules_;
    std::unordered_map<AlertChannel, ChannelHandler> channel_handlers_;
    
    // Rate limiting tracking
    struct RateLimitInfo {
        std::vector<std::chrono::system_clock::time_point> recent_alerts;
    };
    std::unordered_map<int, RateLimitInfo> rate_limits_;
    
    // Statistics
    std::atomic<uint64_t> total_alerts_sent_{0};
    std::atomic<uint64_t> total_alerts_suppressed_{0};
    
    // Thread safety
    mutable std::mutex config_mutex_; 
    
    // Worker Thread (Phase D: Qt Threading)
    QThread* worker_thread_{nullptr};
    std::atomic<bool> stop_worker_{false};
    std::mutex queue_mutex_;
    std::vector<CorrelatedEvent> event_queue_;
    
    void processEvent(const CorrelatedEvent& event); // Internal processing logic
};

} // namespace vms::events
