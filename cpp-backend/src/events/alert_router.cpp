// ==============================================================
// File: src/events/alert_router.cpp
// Alert Router Implementation
// ==============================================================

#include "events/alert_router.h"
#include "core/runtime_state.h"
#include "events/zone_manager.h"
#include "utils/background_job_runner.h"
#include "utils/email_sender.h"
#include "utils/logger.h"
#include "streaming/camera_stream_manager_qt.h"
#include "database/db_manager.h"
#include <algorithm>
#include <ctime>
#include <thread>
#include <sstream>
#include <iomanip>
#include <curl/curl.h>

namespace vms::events {

namespace {

vms::utils::BackgroundJobRunner& webhookRunner() {
    static vms::utils::BackgroundJobRunner runner("alert-webhooks", 2, 128);
    return runner;
}

// SMS payload still needs CR/LF stripping for the body and recipient
// fields. Email goes through vms::utils::sendEmailAsync, which calls
// sanitizeMailHeader internally; SMS keeps a thin local alias to avoid
// touching the Twilio call site for this refactor.
inline std::string sanitizeHeader(const std::string& s, std::size_t max_len = 256) {
    return vms::utils::sanitizeMailHeader(s, max_len);
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

    // Drain a batch (up to kDrainBatch) per tick instead of one event per signal.
    // The previous one-event-per-signal pattern paid a Qt::QueuedConnection hop
    // (event-loop wakeup + slot dispatch) per event — at ~50µs/hop this caps
    // throughput around 20k events/s and starves the event loop under burst.
    constexpr size_t kDrainBatch = 16;
    std::vector<CorrelatedEvent> batch;
    batch.reserve(kDrainBatch);

    bool more_pending = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        size_t take = std::min(kDrainBatch, event_queue_.size());
        for (size_t i = 0; i < take; ++i) {
            batch.push_back(std::move(event_queue_.front()));
            event_queue_.erase(event_queue_.begin());
        }
        more_pending = !event_queue_.empty();
    }

    for (auto& e : batch) {
        if (stop_worker_) break;
        processEvent(e);
    }

    if (more_pending && !stop_worker_) {
        Q_EMIT eventQueued();
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

    // BUG-7 FIX: Hỗ trợ overnight range (e.g. start=22, end=06)
    // Rule engine đã xử lý đúng nhưng AlertRouter thì không
    bool in_range;
    if (rule.start_hour <= rule.end_hour) {
        in_range = (hour >= rule.start_hour && hour <= rule.end_hour);
    } else {
        // overnight: e.g. 22:00 → 06:00
        in_range = (hour >= rule.start_hour || hour <= rule.end_hour);
    }
    if (!in_range) return false;

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
    // FIX: đặt type SAU toJSON() để không bị field "type" của event_type overwrite
    j["type"] = "alert";
    j["event_type"] = eventTypeToString(event.type);  // giữ event_type riêng
    j["rule_name"] = rule.name;
    j["message"] = "Quy tắc AI '" + rule.name + "' đã phát hiện: " + eventTypeToString(event.type);

    // FIX LỖI 1: broadcast đến ĐÚNG camera_id của event
    // (broadcastEvent(0, j) chỉ gửi đến clients subscribe camera_id=0 — không có client nào)
    // Bây giờ gửi đến camera cụ thể VÀ global (camera_id=0) để đảm bảo frontend nhận được
    int cam_id = event.best_camera_id > 0 ? event.best_camera_id
                  : (!event.camera_ids.empty() ? event.camera_ids[0] : 0);
    auto& ws = vms::streaming::CameraStreamManager::getInstance();
    if (cam_id > 0) {
        ws.broadcastEvent(cam_id, j);  // đến clients đang xem camera này
    }
    ws.broadcastEvent(0, j);           // đến clients subscribe global channel
    LOG_INFO("[AlertRouter] UI notification broadcasted for event ID: {} camera: {}", event.correlation_id, cam_id);
}

// Subject + body composition stays here (rule-aware); SMTP transport is in
// vms::utils::sendEmailAsync (see utils/email_sender.h). Both the legacy
// AlertManager path and this RuleEngine path share the helper so SMTP
// settings, header sanitisation, queueing and STARTTLS are maintained in
// exactly one place.
void AlertRouter::sendEmail(const CorrelatedEvent& event, const AlertRule& rule) {
    if (rule.email_addresses.empty()) return;

    const std::string subject =
        "[VMS] " + std::string(eventSeverityToString(event.severity)) +
        " — " + std::string(eventTypeToString(event.type)) +
        (rule.name.empty() ? "" : (" (" + rule.name + ")"));

    // Body: keep it plain text + bounded. event.toJSON() can be large
    // (snapshots etc.) — the helper caps the body at 8 KB internally, but
    // we also dump-and-trim here so the rule preface stays on top.
    std::string body_json = event.toJSON().dump(2);
    if (body_json.size() > 6144) body_json.resize(6144);
    std::string body =
        "VMS AI Alert\r\n\r\n"
        "Rule: " + vms::utils::sanitizeMailHeader(rule.name, 200) + "\r\n"
        "Type: " + std::string(eventTypeToString(event.type)) + "\r\n"
        "Severity: " + std::string(eventSeverityToString(event.severity)) + "\r\n"
        "Camera: " + std::to_string(event.best_camera_id) + "\r\n\r\n"
        "Event payload:\r\n" + body_json + "\r\n";

    if (vms::utils::sendEmailAsync(subject, body, rule.email_addresses, "alert_router")) {
        LOG_INFO("[AlertRouter] Email queued for {} recipient(s): {}",
                 rule.email_addresses.size(), eventTypeToString(event.type));
    }
}

// SMS via Twilio HTTPS API. Settings:
//   twilio_account_sid, twilio_auth_token, twilio_from_number
// If any is missing we WARN and skip (feature-disabled).
void AlertRouter::sendSMS(const CorrelatedEvent& event, const AlertRule& rule) {
    if (rule.phone_numbers.empty()) return;

    auto& db = vms::database::DbManager::getInstance();
    const std::string sid   = db.getSetting("twilio_account_sid", "");
    const std::string token = db.getSetting("twilio_auth_token", "");
    const std::string from  = db.getSetting("twilio_from_number", "");
    if (sid.empty() || token.empty() || from.empty()) {
        LOG_WARN("[AlertRouter] Twilio not configured. {} SMS skipped.", rule.phone_numbers.size());
        return;
    }

    // Twilio caps body at 1600 chars; we keep it short so it stays in 1-2 segments.
    std::string body = "[VMS] " + std::string(eventSeverityToString(event.severity)) + " " +
                       std::string(eventTypeToString(event.type)) +
                       " cam=" + std::to_string(event.best_camera_id) +
                       " rule=" + sanitizeHeader(rule.name, 64);
    if (body.size() > 320) body.resize(320);

    const std::string url = "https://api.twilio.com/2010-04-01/Accounts/" + sid + "/Messages.json";

    LOG_INFO("[AlertRouter] SMS queued for {} recipient(s): {}",
             rule.phone_numbers.size(), eventTypeToString(event.type));

    for (const auto& raw_to : rule.phone_numbers) {
        const std::string to = sanitizeHeader(raw_to, 32);
        if (to.empty()) continue;

        if (!webhookRunner().submit([url, sid, token, from, to, body]() {
            if (vms::core::shutting_down.load(std::memory_order_acquire)) return;
            CURL* curl = curl_easy_init();
            if (!curl) return;

            char* enc_to   = curl_easy_escape(curl, to.c_str(), 0);
            char* enc_from = curl_easy_escape(curl, from.c_str(), 0);
            char* enc_body = curl_easy_escape(curl, body.c_str(), 0);
            std::string form = "To=" + std::string(enc_to ? enc_to : "") +
                               "&From=" + std::string(enc_from ? enc_from : "") +
                               "&Body=" + std::string(enc_body ? enc_body : "");
            curl_free(enc_to); curl_free(enc_from); curl_free(enc_body);

            const std::string userpass = sid + ":" + token;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpass.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)form.size());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

            CURLcode res = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
                LOG_ERROR("[AlertRouter] Twilio SMS to {} failed: curl={} http={}",
                          to, curl_easy_strerror(res), http_code);
            } else {
                LOG_INFO("[AlertRouter] SMS delivered to {} (http={})", to, http_code);
            }
            curl_easy_cleanup(curl);
        })) {
            LOG_THROTTLED_WARN(5000, "[AlertRouter] SMS queue full, dropping send");
        }
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

    // [MUST-FIX] SSRF Protection: Blacklist private IP ranges
    // This is a simplified check. In production, resolve the hostname and check the IP.
    if (url.find("://localhost") != std::string::npos ||
        url.find("://127.0.0.1") != std::string::npos ||
        url.find("://192.168.") != std::string::npos ||
        url.find("://10.") != std::string::npos ||
        url.find("://172.16.") != std::string::npos ||
        url.find("://169.254.169.254") != std::string::npos) {
        LOG_ERROR("[AlertRouter] Webhook rejected: Internal/Private IP access denied (SSRF Protection): {}", url);
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

// Most production deployments use a network-callable relay (HTTP/HTTPS) rather
// than local GPIO — local GPIO requires a Pi-class deployment and per-board
// hardware code. We support both: if `alarm_output_url` is configured, POST a
// JSON payload to it via the existing async webhook runner. SSRF restrictions
// from sendWebhook do NOT apply here: the alarm relay typically lives on the
// same LAN as the VMS box, so private-IP destinations are EXPECTED.
void AlertRouter::triggerAlarmOutput(const CorrelatedEvent& event, const AlertRule& rule) {
    auto& db = vms::database::DbManager::getInstance();
    const std::string url = db.getSetting("alarm_output_url", "");
    if (url.empty()) {
        LOG_WARN("[AlertRouter] alarm_output_url not configured; alarm trigger logged only "
                 "(event {})", event.correlation_id);
        return;
    }

    bool is_http  = url.rfind("http://", 0)  == 0;
    bool is_https = url.rfind("https://", 0) == 0;
    if (!is_http && !is_https) {
        LOG_ERROR("[AlertRouter] alarm_output_url must be http(s)://. Got: {}", url);
        return;
    }

    nlohmann::json payload = {
        {"action", "alarm_trigger"},
        {"event_id", event.correlation_id},
        {"event_type", eventTypeToString(event.type)},
        {"severity", eventSeverityToString(event.severity)},
        {"camera_id", event.best_camera_id},
        {"rule_id", rule.rule_id},
        {"rule_name", rule.name}
    };

    LOG_INFO("[AlertRouter] Alarm output triggered for event {} → {}", event.correlation_id, url);
    executeAsyncPost(url, payload.dump());
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
