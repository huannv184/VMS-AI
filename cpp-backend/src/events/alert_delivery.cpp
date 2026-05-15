// ==============================================================
// File: src/events/alert_delivery.cpp
// Single delivery layer — replaces AlertManager + AlertRouter (see header).
//
// Concurrency: callable from any producer thread (EventManager hot path,
// ZMQ bridge thread, brand event services). Every channel send is queued
// to a shared bounded background runner — no blocking I/O on the caller.
// ==============================================================

#include "events/alert_delivery.h"
#include "events/rule_engine.h"
#include "core/runtime_state.h"
#include "database/db_manager.h"
#include "streaming/camera_stream_manager_qt.h"
#include "utils/background_job_runner.h"
#include "utils/email_sender.h"
#include "utils/logger.h"
#include "utils/url_validator.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace vms::events {

namespace {

vms::utils::BackgroundJobRunner& deliveryRunner() {
    // 2 workers / 128 queue mirrors the AlertManager + AlertRouter pools we
    // are replacing. Drop-on-full keeps the producer hot path bounded; if a
    // remote endpoint stalls (15s libcurl timeout), workers free up before
    // the queue fills under any reasonable event rate.
    static vms::utils::BackgroundJobRunner instance("alert-delivery", 2, 128);
    return instance;
}

inline std::string sanitizeHeader(const std::string& s, std::size_t max_len = 256) {
    return vms::utils::sanitizeMailHeader(s, max_len);
}

std::vector<std::string> readStringArray(const nlohmann::json& meta, const char* key) {
    std::vector<std::string> out;
    if (!meta.is_object()) return out;
    auto it = meta.find(key);
    if (it == meta.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::string readString(const nlohmann::json& meta, const char* key, const std::string& def = "") {
    if (!meta.is_object()) return def;
    auto it = meta.find(key);
    if (it == meta.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

// HTTP POST via libcurl. resolve_entry pins DNS to the IP isInternalUrl()
// already validated, closing the SSRF DNS-rebind TOCTOU.
void executeAsyncPost(const std::string& url,
                      const std::string& payload,
                      const std::string& resolve_entry = "") {
    if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

    if (!deliveryRunner().submit([url, payload, resolve_entry]() {
        if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

        CURL* curl = curl_easy_init();
        if (!curl) return;

        struct curl_slist* headers = nullptr;
        struct curl_slist* resolve_list = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        if (!resolve_entry.empty()) {
            resolve_list = curl_slist_append(nullptr, resolve_entry.c_str());
            curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
        }

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (res != CURLE_OK) {
            LOG_ERROR("[alert-delivery] POST {} failed: curl={}", url, curl_easy_strerror(res));
        } else if (http_code >= 400) {
            LOG_WARN("[alert-delivery] POST {} returned http={}", url, http_code);
        }

        curl_slist_free_all(headers);
        if (resolve_list) curl_slist_free_all(resolve_list);
        curl_easy_cleanup(curl);
    })) {
        LOG_THROTTLED_WARN(5000, "[alert-delivery] queue full, dropping POST to {}", url);
    }
}

// ── Channel implementations ─────────────────────────────────────────────

void deliverEmail(const CompositeRule& rule, const RuleAction& action, const RawEvent& event) {
    auto recipients = readStringArray(action.metadata, "email_addresses");
    if (recipients.empty()) {
        LOG_THROTTLED_WARN(60000,
            "[alert-delivery] EMAIL channel on rule '{}' has no email_addresses in metadata; skipping",
            rule.name);
        return;
    }

    const std::string subject =
        std::string("[VMS] ") + eventSeverityToString(event.severity) +
        " — " + eventTypeToString(event.type) +
        (rule.name.empty() ? "" : (" (" + rule.name + ")"));

    // Cap event JSON dump so a single huge metadata blob can't push the body
    // past sendEmailAsync's internal 8 KB cap before the rule preface lands.
    std::string body_json = event.toJSON().dump(2);
    if (body_json.size() > 6144) body_json.resize(6144);

    std::ostringstream body;
    body << "VMS AI Alert\r\n\r\n"
         << "Rule: "     << vms::utils::sanitizeMailHeader(rule.name, 200) << "\r\n"
         << "Type: "     << eventTypeToString(event.type) << "\r\n"
         << "Severity: " << eventSeverityToString(event.severity) << "\r\n"
         << "Camera: "   << event.camera_id << "\r\n\r\n"
         << "Event payload:\r\n" << body_json << "\r\n";

    if (vms::utils::sendEmailAsync(subject, body.str(), recipients, "alert_delivery")) {
        LOG_INFO("[alert-delivery] EMAIL queued to {} recipient(s) for rule '{}'",
                 recipients.size(), rule.name);
    }
}

void deliverSMS(const CompositeRule& rule, const RuleAction& action, const RawEvent& event) {
    auto numbers = readStringArray(action.metadata, "phone_numbers");
    if (numbers.empty()) {
        LOG_THROTTLED_WARN(60000,
            "[alert-delivery] SMS channel on rule '{}' has no phone_numbers in metadata; skipping",
            rule.name);
        return;
    }

    auto& db = vms::database::DbManager::getInstance();
    const std::string sid   = db.getSetting("twilio_account_sid", "");
    const std::string token = db.getSetting("twilio_auth_token", "");
    const std::string from  = db.getSetting("twilio_from_number", "");
    if (sid.empty() || token.empty() || from.empty()) {
        LOG_WARN("[alert-delivery] Twilio not configured. {} SMS skipped for rule '{}'.",
                 numbers.size(), rule.name);
        return;
    }

    std::string body = std::string("[VMS] ") + eventSeverityToString(event.severity) + " " +
                       eventTypeToString(event.type) +
                       " cam=" + std::to_string(event.camera_id) +
                       " rule=" + sanitizeHeader(rule.name, 64);
    if (body.size() > 320) body.resize(320);

    const std::string url = "https://api.twilio.com/2010-04-01/Accounts/" + sid + "/Messages.json";

    LOG_INFO("[alert-delivery] SMS queued for {} recipient(s): {}",
             numbers.size(), eventTypeToString(event.type));

    for (const auto& raw_to : numbers) {
        const std::string to = sanitizeHeader(raw_to, 32);
        if (to.empty()) continue;

        if (!deliveryRunner().submit([url, sid, token, from, to, body]() {
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
                LOG_ERROR("[alert-delivery] Twilio SMS to {} failed: curl={} http={}",
                          to, curl_easy_strerror(res), http_code);
            } else {
                LOG_INFO("[alert-delivery] SMS delivered to {} (http={})", to, http_code);
            }
            curl_easy_cleanup(curl);
        })) {
            LOG_THROTTLED_WARN(5000, "[alert-delivery] SMS queue full, dropping send");
        }
    }
}

// Picks `url_override` first (used by ALERT actions that put webhook_url in
// metadata), otherwise falls back to action.webhook_url (the WEBHOOK action
// case). Skips silently when neither set — caller already logged.
void deliverWebhook(const CompositeRule& rule, const RuleAction& action, const RawEvent& event) {
    std::string url = readString(action.metadata, "webhook_url");
    if (url.empty()) url = action.webhook_url;

    if (url.empty()) {
        LOG_THROTTLED_WARN(60000,
            "[alert-delivery] WEBHOOK channel on rule '{}' has no URL (action.webhook_url + metadata.webhook_url both empty)",
            rule.name);
        return;
    }

    bool is_http  = url.rfind("http://", 0) == 0;
    bool is_https = url.rfind("https://", 0) == 0;
    if (!is_http && !is_https) {
        LOG_ERROR("[alert-delivery] Webhook rejected — URL must be http(s)://. Got: {}", url);
        return;
    }

    auto v = vms::utils::isInternalUrl(url);
    if (v.internal) {
        LOG_ERROR("[alert-delivery] Webhook rejected (SSRF guard): host='{}' resolved='{}' reason={}",
                  v.host, v.resolved_ip, v.reason);
        return;
    }

    nlohmann::json payload;
    payload["event_id"]   = event.event_id;
    payload["camera_id"]  = event.camera_id;
    payload["event_type"] = eventTypeToString(event.type);
    payload["severity"]   = eventSeverityToString(event.severity);
    payload["confidence"] = event.confidence;
    payload["zone_id"]    = event.zone_id;
    payload["object_class"] = event.object_class;
    payload["timestamp"]  = std::chrono::system_clock::to_time_t(event.timestamp);
    payload["snapshot_path"] = event.snapshot_path;
    payload["rule_id"]    = rule.rule_id;
    payload["rule_name"]  = rule.name;
    if (!event.metadata.is_null()) payload["metadata"] = event.metadata;

    LOG_INFO("[alert-delivery] WEBHOOK to {} (rule '{}')", url, rule.name);
    executeAsyncPost(url, payload.dump(), vms::utils::buildResolveEntry(v));
}

void deliverUINotification(const CompositeRule& rule, const RuleAction& /*action*/, const RawEvent& event) {
    // EventManager::createEvent already broadcasts a base "alert" event to
    // both the camera-scoped channel and global. This is the *rule-aware*
    // companion message — frontend can show "Quy tắc <name> đã kích hoạt"
    // on top of the raw alarm. Same WS infra, same delivery guarantees.
    nlohmann::json payload;
    payload["type"]        = "alert";
    payload["event_type"]  = eventTypeToString(event.type);
    payload["event_id"]    = event.event_id;
    payload["camera_id"]   = event.camera_id;
    payload["severity"]    = eventSeverityToString(event.severity);
    payload["rule_id"]     = rule.rule_id;
    payload["rule_name"]   = rule.name;
    payload["message"]     = "Quy tắc AI '" + rule.name + "' đã phát hiện: " +
                             eventTypeToString(event.type);
    payload["timestamp"]   = std::chrono::system_clock::to_time_t(event.timestamp);
    payload["snapshot_url"] = event.snapshot_path;

    try {
        auto& ws = vms::streaming::CameraStreamManager::getInstance();
        if (event.camera_id > 0) ws.broadcastEvent(event.camera_id, payload);
        ws.broadcastEvent(0, payload);
    } catch (const std::exception& e) {
        LOG_THROTTLED_ERROR(5000, "[alert-delivery] UI broadcast failed: {}", e.what());
    }
}

void deliverTelegram(const CompositeRule& rule, const RuleAction& action, const RawEvent& event) {
    auto& db = vms::database::DbManager::getInstance();
    std::string bot_token = db.getSetting("telegram_bot_token", "");
    // Allow per-rule chat override via metadata; fall back to global setting.
    std::string chat_id = readString(action.metadata, "telegram_chat_id");
    if (chat_id.empty()) chat_id = db.getSetting("telegram_chat_id", "");

    if (bot_token.empty() || chat_id.empty()) {
        LOG_WARN("[alert-delivery] Telegram not configured (bot_token/chat_id missing). Rule '{}' skipped.",
                 rule.name);
        return;
    }

    std::string url = "https://api.telegram.org/bot" + bot_token + "/sendMessage";

    std::string text = "🚨 VMS AI ALERT 🚨\n\n";
    text += "Tín hiệu: " + std::string(eventTypeToString(event.type)) + "\n";
    text += "Camera: "   + std::to_string(event.camera_id) + "\n";
    text += "Luật: "     + rule.name + "\n";
    text += "Mức độ: "   + std::string(eventSeverityToString(event.severity)) + "\n";

    nlohmann::json payload;
    payload["chat_id"]    = chat_id;
    payload["text"]       = text;
    payload["parse_mode"] = "Markdown";

    // Even though api.telegram.org is hardcoded, run through SSRF guard so
    // /etc/hosts or resolver poisoning can't redirect bot-token POSTs at LAN.
    auto v = vms::utils::isInternalUrl(url);
    if (v.internal) {
        LOG_ERROR("[alert-delivery] Telegram URL refused by SSRF guard: host='{}' resolved='{}' reason={}",
                  v.host, v.resolved_ip, v.reason);
        return;
    }

    LOG_INFO("[alert-delivery] Telegram queued to chat {} for rule '{}'", chat_id, rule.name);
    executeAsyncPost(url, payload.dump(), vms::utils::buildResolveEntry(v));
}

void deliverAlarmOutput(const CompositeRule& rule, const RuleAction& /*action*/, const RawEvent& event) {
    auto& db = vms::database::DbManager::getInstance();
    const std::string url = db.getSetting("alarm_output_url", "");
    if (url.empty()) {
        LOG_WARN("[alert-delivery] alarm_output_url not configured; alarm trigger logged only "
                 "(rule '{}', event {})", rule.name, event.event_id);
        return;
    }

    bool is_http  = url.rfind("http://", 0) == 0;
    bool is_https = url.rfind("https://", 0) == 0;
    if (!is_http && !is_https) {
        LOG_ERROR("[alert-delivery] alarm_output_url must be http(s)://. Got: {}", url);
        return;
    }

    nlohmann::json payload = {
        {"action",     "alarm_trigger"},
        {"event_id",   event.event_id},
        {"event_type", eventTypeToString(event.type)},
        {"severity",   eventSeverityToString(event.severity)},
        {"camera_id",  event.camera_id},
        {"rule_id",    rule.rule_id},
        {"rule_name",  rule.name}
    };

    // ALARM_OUTPUT typically points at a LAN relay — SSRF guard would refuse
    // private destinations, which is exactly what we expect here. Skip it.
    LOG_INFO("[alert-delivery] Alarm output triggered for event {} → {}", event.event_id, url);
    executeAsyncPost(url, payload.dump());
}

void dispatchChannels(const CompositeRule& rule, const RuleAction& action, const RawEvent& event) {
    for (auto channel : action.channels) {
        switch (channel) {
            case AlertChannel::UI_NOTIFICATION: deliverUINotification(rule, action, event); break;
            case AlertChannel::EMAIL:           deliverEmail(rule, action, event); break;
            case AlertChannel::SMS:             deliverSMS(rule, action, event); break;
            case AlertChannel::WEBHOOK:         deliverWebhook(rule, action, event); break;
            case AlertChannel::MOBILE_PUSH:     deliverTelegram(rule, action, event); break;
            case AlertChannel::ALARM_OUTPUT:    deliverAlarmOutput(rule, action, event); break;
            case AlertChannel::NONE:            break;
        }
    }
}

} // namespace

void deliverAction(const CompositeRule& rule, const RuleAction& action, const RawEvent& event) {
    switch (action.type) {
        case ActionType::ALERT:
            // Multi-channel fan-out per action.channels.
            if (action.channels.empty()) {
                LOG_THROTTLED_WARN(60000,
                    "[alert-delivery] ALERT action on rule '{}' has no channels; nothing to deliver",
                    rule.name);
                return;
            }
            dispatchChannels(rule, action, event);
            break;

        case ActionType::WEBHOOK:
            // Standalone webhook action — single channel implied.
            deliverWebhook(rule, action, event);
            break;

        // RECORD_CLIP / SNAPSHOT / LOG are handled inline by RuleEngine
        // (they touch PipelineManager / SnapshotManager directly). Nothing
        // for the network-delivery layer to do.
        case ActionType::RECORD_CLIP:
        case ActionType::SNAPSHOT:
        case ActionType::LOG:
            break;
    }
}

void shutdownDelivery() {
    deliveryRunner().shutdown();
}

} // namespace vms::events
