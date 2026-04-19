// ==============================================================
// File: src/events/alert_router.cpp
// Alert Router Implementation
// ==============================================================

#include "events/alert_router.h"
#include "core/runtime_state.h"
#include "events/zone_manager.h"
#include "utils/background_job_runner.h"
#include "utils/logger.h"
#include "streaming/camera_stream_manager_qt.h"
#include "database/db_manager.h"
#include <algorithm>
#include <ctime>
#include <thread>
#include <curl/curl.h>

namespace vms::events {

namespace {

vms::utils::BackgroundJobRunner& webhookRunner() {
    static vms::utils::BackgroundJobRunner runner("alert-webhooks", 2, 128);
    return runner;
}

}

// ============================================================================
// STRING CONVERSION
// ============================================================================

const char* alertChannelToString(AlertChannel channel) {
    switch (channel) {
        case AlertChannel::UI_NOTIFICATION: return "UI_NOTIFICATION";
        case AlertChannel::EMAIL: return "EMAIL";
        case AlertChannel::SMS: return "SMS";
        case AlertChannel::WEBHOOK: return "WEBHOOK";
        case AlertChannel::MOBILE_PUSH: return "MOBILE_PUSH";
        case AlertChannel::ALARM_OUTPUT: return "ALARM_OUTPUT";
        default: return "NONE";
    }
}

AlertChannel stringToAlertChannel(const std::string& str) {
    if (str == "UI_NOTIFICATION") return AlertChannel::UI_NOTIFICATION;
    if (str == "EMAIL") return AlertChannel::EMAIL;
    if (str == "SMS") return AlertChannel::SMS;
    if (str == "WEBHOOK") return AlertChannel::WEBHOOK;
    if (str == "MOBILE_PUSH") return AlertChannel::MOBILE_PUSH;
    if (str == "ALARM_OUTPUT") return AlertChannel::ALARM_OUTPUT;
    return AlertChannel::NONE;
}

// ============================================================================
// ALERT RULE SERIALIZATION
// ============================================================================

nlohmann::json AlertRule::toJSON() const {
    nlohmann::json j;
    
    j["rule_id"] = rule_id;
    j["name"] = name;
    j["enabled"] = enabled;
   
    // Event types
    nlohmann::json types_array = nlohmann::json::array();
    for (auto type : event_types) {
        types_array.push_back(eventTypeToString(type));
    }
    j["event_types"] = types_array;
    
    j["zone_ids"] = zone_ids;
    j["min_severity"] = eventSeverityToString(min_severity);
    
    j["time_restricted"] = time_restricted;
    j["start_hour"] = start_hour;
    j["end_hour"] = end_hour;
    j["days_of_week"] = days_of_week;
    
    // Channels
    nlohmann::json channels_array = nlohmann::json::array();
    for (auto channel : channels) {
        channels_array.push_back(alertChannelToString(channel));
    }
    j["channels"] = channels_array;
    
    j["max_per_hour"] = max_per_hour;
    j["email_addresses"] = email_addresses;
    j["phone_numbers"] = phone_numbers;
    j["webhook_url"] = webhook_url;
    j["metadata"] = metadata;
    
    return j;
}

AlertRule AlertRule::fromJSON(const nlohmann::json& j) {
    AlertRule rule;
    
    rule.rule_id = j.value("rule_id", 0);
    rule.name = j.value("name", "");
    rule.enabled = j.value("enabled", true);
    
    // Event types
    if (j.contains("event_types") && j["event_types"].is_array()) {
        for (const auto& type_str : j["event_types"]) {
            rule.event_types.push_back(stringToEventType(type_str));
        }
    }
    
    if (j.contains("zone_ids")) {
        rule.zone_ids = j["zone_ids"].get<std::vector<int>>();
    }
    
    rule.min_severity = stringToEventSeverity(j.value("min_severity", "MEDIUM"));
    rule.time_restricted = j.value("time_restricted", false);
    rule.start_hour = j.value("start_hour", 0);
    rule.end_hour = j.value("end_hour", 23);
    
    if (j.contains("days_of_week")) {
        rule.days_of_week = j["days_of_week"].get<std::vector<int>>();
    }
    
    // Channels
    if (j.contains("channels") && j["channels"].is_array()) {
        for (const auto& channel_str : j["channels"]) {
            rule.channels.push_back(stringToAlertChannel(channel_str));
        }
    }
    
    rule.max_per_hour = j.value("max_per_hour", 60);
    
    if (j.contains("email_addresses")) {
        rule.email_addresses = j["email_addresses"].get<std::vector<std::string>>();
    }
    
    if (j.contains("phone_numbers")) {
        rule.phone_numbers = j["phone_numbers"].get<std::vector<std::string>>();
    }
    
    rule.webhook_url = j.value("webhook_url", "");
    
    if (j.contains("metadata")) {
        rule.metadata = j["metadata"];
    }
    
    return rule;
}

// ============================================================================
// ALERT ROUTER
// ============================================================================

AlertRouter& AlertRouter::getInstance() {
    static AlertRouter instance;
    return instance;
}

AlertRouter::AlertRouter() {
    worker_thread_ = new QThread();
    this->moveToThread(worker_thread_);
    
    // Connect the local signal to the slot to process the queue in the worker thread
    connect(this, &AlertRouter::eventQueued, this, &AlertRouter::processQueue, Qt::QueuedConnection);
    
    worker_thread_->start();
    LOG_INFO("AlertRouter worker thread started (QThread)");
}

AlertRouter::~AlertRouter() {
    shutdown();
}

void AlertRouter::routeEvent(const CorrelatedEvent& event) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (event_queue_.size() < 1000) { // Max queue size to prevent OOM
             event_queue_.push_back(event);
        } else {
             LOG_WARN("[AlertRouter] Event queue full! Dropping event.");
        }
    }
    Q_EMIT eventQueued();
}

void AlertRouter::processQueue() {
    if (stop_worker_) return;
    
    CorrelatedEvent event;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!event_queue_.empty()) {
            event = event_queue_.front();
            event_queue_.erase(event_queue_.begin());
        } else {
            return;
        }
    }
    
    // Process outside queue lock
    processEvent(event);
    
    // If more items in queue, emit signal again to keep processing
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!event_queue_.empty() && !stop_worker_) {
            Q_EMIT eventQueued();
        }
    }
}

void AlertRouter::processEvent(const CorrelatedEvent& event) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    // LOG_DEBUG("[AlertRouter] Processing event: {}", event.correlation_id); // Reduced log
    
    for (auto& rule : rules_) {
        if (!rule.enabled) continue;
        
        // Evaluate rule against event
        if (evaluateRule(rule, event)) {
            // Check rate limit
            if (!withinRateLimit(rule.rule_id)) {
                // LOG_WARN("[AlertRouter] Rule {} suppressed by rate limit", rule.rule_id);
                total_alerts_suppressed_++;
                continue;
            }
            
            // Send to configured channels
            sendToChannels(event, rule);
            
            // Record alert
            recordAlert(rule.rule_id);
            total_alerts_sent_++;
            
            LOG_INFO("[AlertRouter] Alert sent via rule {}: {}", rule.rule_id, rule.name);
        }
    }
}

bool AlertRouter::addRule(const AlertRule& rule) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    // Check if ID exists
    for (const auto& existing : rules_) {
        if (existing.rule_id == rule.rule_id) {
            LOG_WARN("[AlertRouter] Rule {} already exists", rule.rule_id);
            return false;
        }
    }
    
    rules_.push_back(rule);
    LOG_INFO("[AlertRouter] Added rule {}: {}", rule.rule_id, rule.name);
    return true;
}

bool AlertRouter::removeRule(int rule_id) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    auto it = std::find_if(rules_.begin(), rules_.end(),
        [rule_id](const AlertRule& r) { return r.rule_id == rule_id; });
    
    if (it == rules_.end()) {
        LOG_WARN("[AlertRouter] Rule {} not found", rule_id);
        return false;
    }
    
    rules_.erase(it);
    rate_limits_.erase(rule_id);
    
    LOG_INFO("[AlertRouter] Removed rule {}", rule_id);
    return true;
}

bool AlertRouter::updateRule(const AlertRule& rule) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    auto it = std::find_if(rules_.begin(), rules_.end(),
        [&rule](const AlertRule& r) { return r.rule_id == rule.rule_id; });
    
    if (it == rules_.end()) {
        LOG_WARN("[AlertRouter] Rule {} not found for update", rule.rule_id);
        return false;
    }
    
    *it = rule;
    LOG_INFO("[AlertRouter] Updated rule {}", rule.rule_id);
    return true;
}

AlertRule* AlertRouter::getRule(int rule_id) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    for (auto& rule : rules_) {
        if (rule.rule_id == rule_id) {
            return &rule;
        }
    }
    
    return nullptr;
}

std::vector<AlertRule> AlertRouter::getAllRules() {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return rules_;
}

void AlertRouter::registerChannelHandler(AlertChannel channel, ChannelHandler handler) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    channel_handlers_[channel] = handler;
    LOG_INFO("[AlertRouter] Registered handler for channel: {}", 
            alertChannelToString(channel));
}

bool AlertRouter::testAlert(int rule_id) {
    // Note: getRule locks internally, but returning pointer is unsafe if rule is deleted?
    // Actually getRule locks, returns pointer, unlocks. Pointer is to element in vector.
    // If vector reallocates, pointer invalid. 
    // This is a preexisting issue, but let's stick to simple fix:
    // We should copy the rule or lock for duration. 
    // For test, we can just use processEvent?
    
    AlertRule rule_copy;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        // Iterate directly to avoid calling getRule() which locks recursively
        for (const auto& r : rules_) {
            if (r.rule_id == rule_id) {
                rule_copy = r;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        LOG_ERROR("[AlertRouter] Rule {} not found for testing", rule_id);
        return false;
    }
    
    // Create test event
    CorrelatedEvent test_event;
    test_event.correlation_id = 9999;
    test_event.type = EventType::PERSON_DETECTED;
    test_event.severity = EventSeverity::MEDIUM;
    test_event.primary_zone_id = 1;
    test_event.event_count = 1;
    test_event.first_seen = std::chrono::system_clock::now();
    test_event.last_seen = test_event.first_seen;
    
    LOG_INFO("[AlertRouter] Testing rule {}: {}", rule_id, rule_copy.name);
    // Directly process (synchronous for test)
    // We can reuse sendToChannels but it needs rule reference.
    // sendToChannels does NOT lock.
    sendToChannels(test_event, rule_copy);
    
    return true;
}

nlohmann::json AlertRouter::getStatistics() {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    nlohmann::json stats;
    stats["total_rules"] = rules_.size();
    stats["total_alerts_sent"] = total_alerts_sent_.load();
    stats["total_alerts_suppressed"] = total_alerts_suppressed_.load();
    
    // Per-rule stats
    nlohmann::json rule_stats = nlohmann::json::array();
    for (const auto& rule : rules_) {
        nlohmann::json r;
        r["rule_id"] = rule.rule_id;
        r["name"] = rule.name;
        r["enabled"] = rule.enabled;
        
        auto it = rate_limits_.find(rule.rule_id);
        if (it != rate_limits_.end()) {
            r["alerts_last_hour"] = it->second.recent_alerts.size();
        } else {
            r["alerts_last_hour"] = 0;
        }
        
        rule_stats.push_back(r);
    }
    stats["rules"] = rule_stats;
    
    return stats;
}

// ============================================================================
// RULE EVALUATION
// ============================================================================

bool AlertRouter::evaluateRule(const AlertRule& rule, const CorrelatedEvent& event) {
    // Check event type
    if (!rule.event_types.empty()) {
        if (std::find(rule.event_types.begin(), rule.event_types.end(), 
                     event.type) == rule.event_types.end()) {
            return false;
        }
    }
    
    // Check severity
    if (event.severity < rule.min_severity) {
        return false;
    }
    
    // Check zones
    if (!rule.zone_ids.empty()) {
        bool found = false;
        for (int zone : event.all_zones) {
            if (std::find(rule.zone_ids.begin(), rule.zone_ids.end(), 
                         zone) != rule.zone_ids.end()) {
                found = true;
                break;
            }
        }
        if (!found && event.primary_zone_id >= 0) {
            if (std::find(rule.zone_ids.begin(), rule.zone_ids.end(),
                         event.primary_zone_id) != rule.zone_ids.end()) {
                found = true;
            }
        }
        if (!found) return false;
    }
    
    // Check time restrictions
    if (rule.time_restricted) {
        if (!isWithinTimeWindow(rule)) {
            return false;
        }
    }
    
    return true;
}

bool AlertRouter::isWithinTimeWindow(const AlertRule& rule) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = std::localtime(&time_t);
    
    int hour = tm->tm_hour;
    int day = tm->tm_wday;
    
    // Check hour
    if (hour < rule.start_hour || hour > rule.end_hour) {
        return false;
    }
    
    // Check day of week
    if (!rule.days_of_week.empty()) {
        if (std::find(rule.days_of_week.begin(), rule.days_of_week.end(), 
                     day) == rule.days_of_week.end()) {
            return false;
        }
    }
    
    return true;
}

bool AlertRouter::withinRateLimit(int rule_id) {
    // NOTE: This function is called inside processEvent which already holds config_mutex_.
    // We MUST NOT call getRule() here (which also locks config_mutex_) — that causes deadlock.
    // Instead, search rules_ directly without re-locking.
    auto& rate_info = rate_limits_[rule_id];
    auto now = std::chrono::system_clock::now();
    auto one_hour_ago = now - std::chrono::hours(1);
    
    // Remove old alerts
    rate_info.recent_alerts.erase(
        std::remove_if(rate_info.recent_alerts.begin(), 
                      rate_info.recent_alerts.end(),
                      [one_hour_ago](const auto& t) { return t < one_hour_ago; }),
        rate_info.recent_alerts.end()
    );
    
    // FIX: Search rules_ inline — avoids re-locking config_mutex_ (deadlock risk)
    int max_per_hour = 60; // default
    for (const auto& r : rules_) {
        if (r.rule_id == rule_id) {
            max_per_hour = r.max_per_hour;
            break;
        }
    }
    
    return rate_info.recent_alerts.size() < static_cast<size_t>(max_per_hour);
}

void AlertRouter::recordAlert(int rule_id) {
    auto& rate_info = rate_limits_[rule_id];
    rate_info.recent_alerts.push_back(std::chrono::system_clock::now());
}

// ============================================================================
// CHANNEL ROUTING
// ============================================================================

void AlertRouter::sendToChannels(const CorrelatedEvent& event, const AlertRule& rule) {
    for (auto channel : rule.channels) {
        sendToChannel(channel, event, rule);
    }
}

void AlertRouter::sendToChannel(AlertChannel channel, const CorrelatedEvent& event, 
                                 const AlertRule& rule) {
    // Check for custom handler
    auto it = channel_handlers_.find(channel);
    if (it != channel_handlers_.end()) {
        try {
            it->second(event, rule);
            return;
        } catch (const std::exception& e) {
            LOG_ERROR("[AlertRouter] Custom handler error for {}: {}", 
                     alertChannelToString(channel), e.what());
        }
    }
    
    // Use built-in handlers
    switch (channel) {
        case AlertChannel::UI_NOTIFICATION:
            sendUINotification(event, rule);
            break;
        case AlertChannel::EMAIL:
            sendEmail(event, rule);
            break;
        case AlertChannel::SMS:
            sendSMS(event, rule);
            break;
        case AlertChannel::WEBHOOK:
            sendWebhook(event, rule);
            break;
        case AlertChannel::MOBILE_PUSH:
            sendMobilePush(event, rule);
            break;
        case AlertChannel::ALARM_OUTPUT:
            triggerAlarmOutput(event, rule);
            break;
        default:
            break;
    }
}

void AlertRouter::sendUINotification(const CorrelatedEvent& event, const AlertRule& rule) {
    auto j = event.toJSON();
    j["type"] = "alert";
    j["rule_name"] = rule.name;
    j["message"] = "Quy tắc AI '" + rule.name + "' đã phát hiện: " + eventTypeToString(event.type);
    
    // Broadcast to global WS channel via CameraStreamManager
    vms::streaming::CameraStreamManager::getInstance().broadcastEvent(0, j);
    LOG_INFO("[AlertRouter] UI notification broadcasted for event ID: {}", event.correlation_id);
}

void AlertRouter::sendEmail(const CorrelatedEvent& event, const AlertRule& rule) {
    // TODO: SMTP integration
    LOG_INFO("[AlertRouter] Email to {} recipients: {}", 
            rule.email_addresses.size(), eventTypeToString(event.type));
    
    for (const auto& email : rule.email_addresses) {
        LOG_INFO("[AlertRouter]   → {}", email);
    }
}

void AlertRouter::sendSMS(const CorrelatedEvent& event, const AlertRule& rule) {
    // TODO: SMS gateway integration (Twilio, etc.)
    LOG_INFO("[AlertRouter] SMS to {} recipients: {}",
            rule.phone_numbers.size(), eventTypeToString(event.type));
    
    for (const auto& phone : rule.phone_numbers) {
        LOG_INFO("[AlertRouter]   → {}", phone);
    }
}

// Helper to handle curl post on a detached thread
static void executeAsyncPost(const std::string& url, const std::string& payload) {
    if (vms::core::shutting_down.load(std::memory_order_acquire)) {
        return;
    }

    if (!webhookRunner().submit([url, payload]() {
        CURL* curl = curl_easy_init();
        if (!curl) return;
        
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            LOG_ERROR("[AlertRouter] HTTP post failed: {}", curl_easy_strerror(res));
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    })) {
        LOG_THROTTLED_WARN(5000, "[AlertRouter] Webhook queue full, dropping async post");
    }
}

void AlertRouter::sendWebhook(const CorrelatedEvent& event, const AlertRule& rule) {
    if (rule.webhook_url.empty()) return;

    // Validate URL scheme to prevent command injection
    const std::string& url = rule.webhook_url;
    bool is_http  = url.rfind("http://",  0) == 0;
    bool is_https = url.rfind("https://", 0) == 0;
    if (!is_http && !is_https) {
        LOG_ERROR("[AlertRouter] Webhook rejected: URL must start with http:// or https://. Got: {}", url);
        return;
    }
    
    LOG_INFO("[AlertRouter] Webhook to {}: {}", rule.webhook_url, eventTypeToString(event.type));
    executeAsyncPost(rule.webhook_url, event.toJSON().dump());
}

void AlertRouter::sendMobilePush(const CorrelatedEvent& event, const AlertRule& rule) {
    // Implement Telegram via MobilePush channel
    auto& db = vms::database::DbManager::getInstance();
    std::string bot_token = db.getSetting("telegram_bot_token", "");
    std::string chat_id = db.getSetting("telegram_chat_id", "");
    
    if (bot_token.empty() || chat_id.empty()) {
        LOG_WARN("[AlertRouter] Telegram not configured. Skipping.");
        return;
    }
    
    std::string url = "https://api.telegram.org/bot" + bot_token + "/sendMessage";
    
    std::string text = "🚨 **VMS AI ALERT** 🚨\n\n";
    text += "Tín hiệu: " + std::string(eventTypeToString(event.type)) + "\n";
    text += "Camera: " + std::to_string(event.camera_ids.empty() ? -1 : event.camera_ids[0]) + "\n";
    text += "Luật: " + rule.name + "\n";
    text += "Mức độ: " + std::string(eventSeverityToString(event.severity)) + "\n";
    
    nlohmann::json payload;
    payload["chat_id"] = chat_id;
    payload["text"] = text;
    payload["parse_mode"] = "Markdown";
    
    LOG_INFO("[AlertRouter] Sending Telegram notification to chat {}", chat_id);
    executeAsyncPost(url, payload.dump());
}

void AlertRouter::triggerAlarmOutput(const CorrelatedEvent& event, const AlertRule& rule) {
    // TODO: GPIO/relay control
    LOG_INFO("[AlertRouter] Alarm output triggered for event {}", event.correlation_id);
}

void AlertRouter::shutdown() {
    stop_worker_ = true;
    webhookRunner().shutdown();

    if (worker_thread_) {
        worker_thread_->quit();
        worker_thread_->wait();
        delete worker_thread_;
        worker_thread_ = nullptr;
    }
}

} // namespace vms::events
