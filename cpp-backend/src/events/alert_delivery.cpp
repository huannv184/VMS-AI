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
#include "utils/config.h"
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

// 2026-05-17 BUG-ALERT-CASCADE-POOL-01 fix: per-channel pools. A single
// stalled webhook used to hold both workers of the shared 2/128 pool for up
// to 25 s (10 s connect + 15 s total libcurl timeout), queueing SMS / Telegram
// / alarm-relay sends behind it even when those endpoints were healthy. Each
// channel now drains independently:
//   - webhook  4/256 : operator-supplied URL, highest stall risk; bigger
//                      queue absorbs burst, more workers absorb parallel stalls.
//   - sms      2/128 : Twilio per-long-code rate-limit ~1 msg/s is the real
//                      bottleneck — more workers just queue more 429s.
//   - telegram 2/128 : api.telegram.org per-chat ~30 msg/s; 2 workers covers it.
//   - alarm    1/64  : LAN relay, ms-latency typical. Small queue is intentional:
//                      a stale alarm is useless, drop is the right semantic.
// EMAIL stays on the existing `email-sender` pool (utils/email_sender.cpp);
// UI broadcasts go through CameraStreamManager (Qt queued), neither shares
// state with these.
//
// Shutdown: 4 independent pools, no cross-pool wait → no deadlock. Each
// pool's worker lambdas already gate on vms::core::shutting_down so in-flight
// jobs short-circuit; new submits after shutdown are rejected and counted.
// 2026-05-19 BUG-ALERT-CASCADE-POOL-01 phase 2: config-driven sizing.
// Each *Runner() is a Meyers singleton — the workers/queue values read
// from config at FIRST CALL are captured for the program's lifetime.
// Boot order: main.cpp loads Config::loadFromFile well before any event
// delivery path (AI ingestion or HTTP rule fire) executes, so the read
// reflects the operator's settings. Non-positive values fall back to
// the channel's hardcoded default, matching the 2026-05-17 sizing.
inline std::size_t poolWorkers(int cfg, std::size_t fallback) {
    return cfg > 0 ? static_cast<std::size_t>(cfg) : fallback;
}
inline std::size_t poolQueue(int cfg, std::size_t fallback) {
    return cfg > 0 ? static_cast<std::size_t>(cfg) : fallback;
}

vms::utils::BackgroundJobRunner& webhookRunner() {
    const auto& c = vms::Config::getInstance().getAlertDeliveryConfig().webhook;
    static vms::utils::BackgroundJobRunner instance(
        "delivery-webhook", poolWorkers(c.workers, 4), poolQueue(c.queue_size, 256));
    return instance;
}
vms::utils::BackgroundJobRunner& smsRunner() {
    const auto& c = vms::Config::getInstance().getAlertDeliveryConfig().sms;
    static vms::utils::BackgroundJobRunner instance(
        "delivery-sms", poolWorkers(c.workers, 2), poolQueue(c.queue_size, 128));
    return instance;
}
vms::utils::BackgroundJobRunner& telegramRunner() {
    const auto& c = vms::Config::getInstance().getAlertDeliveryConfig().telegram;
    static vms::utils::BackgroundJobRunner instance(
        "delivery-telegram", poolWorkers(c.workers, 2), poolQueue(c.queue_size, 128));
    return instance;
}
vms::utils::BackgroundJobRunner& alarmRunner() {
    const auto& c = vms::Config::getInstance().getAlertDeliveryConfig().alarm;
    static vms::utils::BackgroundJobRunner instance(
        "delivery-alarm", poolWorkers(c.workers, 1), poolQueue(c.queue_size, 64));
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

// HTTP POST via libcurl, executed inside the worker. ssrf_required toggles the
// inside-worker isInternalUrl() check + CURLOPT_RESOLVE pin; set to true for
// operator-supplied URLs (webhook, telegram), false for fully-trusted internal
// destinations (alarm relay).
//
// Pre-2026-05-15 the SSRF check ran on the producer thread to "refuse before
// queueing", but getaddrinfo() blocks for seconds on slow resolvers and the
// ZMQ bridge / brand-event-service threads have NO other thread to ingest
// events while they wait. Burning a worker slot on the few microseconds of a
// refused job is the cheaper trade.
//
// Caller picks the runner so per-channel pools stay isolated (2026-05-17
// BUG-ALERT-CASCADE-POOL-01 split). Webhook goes to webhookRunner(); alarm
// relay has its own inline submit further down because the worker body reads
// settings the webhook path does not.
void postViaWorker(vms::utils::BackgroundJobRunner& runner,
                   const std::string& url,
                   const std::string& payload,
                   bool ssrf_required) {
    if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

    if (!runner.submit([url, payload, ssrf_required]() {
        if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

        std::string resolve_entry;
        if (ssrf_required) {
            auto v = vms::utils::isInternalUrl(url);
            if (v.internal) {
                LOG_ERROR("[alert-delivery] POST rejected (SSRF guard): host='{}' resolved='{}' reason={}",
                          v.host, v.resolved_ip, v.reason);
                return;
            }
            resolve_entry = vms::utils::buildResolveEntry(v);
        }

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
        LOG_THROTTLED_WARN(5000, "[alert-delivery] webhook queue full, dropping POST to {}", url);
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
    // Producer-side validation: cheap stuff only. Twilio settings are read
    // inside the worker (2026-05-15 hot-path audit) so the producer thread
    // doesn't pay 3 DB SELECTs per event.
    auto numbers = readStringArray(action.metadata, "phone_numbers");
    if (numbers.empty()) {
        LOG_THROTTLED_WARN(60000,
            "[alert-delivery] SMS channel on rule '{}' has no phone_numbers in metadata; skipping",
            rule.name);
        return;
    }

    // Sanitise + filter recipients before queueing — no point burning a worker
    // slot to discover all phone numbers were empty after sanitisation.
    std::vector<std::string> to_list;
    to_list.reserve(numbers.size());
    for (const auto& raw : numbers) {
        std::string t = sanitizeHeader(raw, 32);
        if (!t.empty()) to_list.push_back(std::move(t));
    }
    if (to_list.empty()) return;

    std::string body = std::string("[VMS] ") + eventSeverityToString(event.severity) + " " +
                       eventTypeToString(event.type) +
                       " cam=" + std::to_string(event.camera_id) +
                       " rule=" + sanitizeHeader(rule.name, 64);
    if (body.size() > 320) body.resize(320);

    const std::string rule_name = rule.name;

    LOG_INFO("[alert-delivery] SMS queued for {} recipient(s): {}",
             to_list.size(), eventTypeToString(event.type));

    // ONE job per ALERT-fire (not per phone). Loops phones inside the worker,
    // reading Twilio settings once. Pre-fix this enqueued N jobs each
    // capturing sid/token/from by value — same effective serial behaviour
    // (2 workers, Twilio rate-limit) at the cost of 3 string copies × N.
    if (!smsRunner().submit([to_list, body, rule_name]() {
        if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

        auto& db = vms::database::DbManager::getInstance();
        const std::string sid   = db.getSetting("twilio_account_sid", "");
        const std::string token = db.getSetting("twilio_auth_token", "");
        const std::string from  = db.getSetting("twilio_from_number", "");
        if (sid.empty() || token.empty() || from.empty()) {
            LOG_WARN("[alert-delivery] Twilio not configured. {} SMS skipped for rule '{}'.",
                     to_list.size(), rule_name);
            return;
        }

        const std::string url = "https://api.twilio.com/2010-04-01/Accounts/" + sid + "/Messages.json";
        const std::string userpass = sid + ":" + token;

        for (const auto& to : to_list) {
            if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

            CURL* curl = curl_easy_init();
            if (!curl) continue;

            char* enc_to   = curl_easy_escape(curl, to.c_str(), 0);
            char* enc_from = curl_easy_escape(curl, from.c_str(), 0);
            char* enc_body = curl_easy_escape(curl, body.c_str(), 0);
            std::string form = "To=" + std::string(enc_to ? enc_to : "") +
                               "&From=" + std::string(enc_from ? enc_from : "") +
                               "&Body=" + std::string(enc_body ? enc_body : "");
            curl_free(enc_to); curl_free(enc_from); curl_free(enc_body);

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
        }
    })) {
        LOG_THROTTLED_WARN(5000, "[alert-delivery] SMS queue full, dropping batch for rule '{}'", rule_name);
    }
}

// Picks metadata.webhook_url first (ALERT actions that put webhook URL in
// channel metadata), otherwise falls back to action.webhook_url (standalone
// WEBHOOK action). Skips silently when neither set. The SSRF check + DNS pin
// happen inside the worker (2026-05-15 audit) — producer-side getaddrinfo
// could block the ZMQ bridge / brand event service for seconds on a slow
// resolver.
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

    LOG_INFO("[alert-delivery] WEBHOOK queued to {} (rule '{}')", url, rule.name);
    postViaWorker(webhookRunner(), url, payload.dump(), /*ssrf_required=*/true);
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
    // Per-rule chat override resolved on producer (cheap json lookup).
    // bot_token + global chat_id fallback + SSRF check run inside the worker
    // (2026-05-15 audit) — api.telegram.org DNS can still take time on first
    // resolve, no reason to block the producer for it.
    std::string per_rule_chat = readString(action.metadata, "telegram_chat_id");
    std::string text = "🚨 VMS AI ALERT 🚨\n\n";
    text += "Tín hiệu: " + std::string(eventTypeToString(event.type)) + "\n";
    text += "Camera: "   + std::to_string(event.camera_id) + "\n";
    text += "Luật: "     + rule.name + "\n";
    text += "Mức độ: "   + std::string(eventSeverityToString(event.severity)) + "\n";

    const std::string rule_name = rule.name;

    if (!telegramRunner().submit([per_rule_chat, text, rule_name]() {
        if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

        auto& db = vms::database::DbManager::getInstance();
        const std::string bot_token = db.getSetting("telegram_bot_token", "");
        std::string chat_id = per_rule_chat;
        if (chat_id.empty()) chat_id = db.getSetting("telegram_chat_id", "");

        if (bot_token.empty() || chat_id.empty()) {
            LOG_WARN("[alert-delivery] Telegram not configured (bot_token/chat_id missing). Rule '{}' skipped.",
                     rule_name);
            return;
        }

        const std::string url = "https://api.telegram.org/bot" + bot_token + "/sendMessage";

        // Even though api.telegram.org is hardcoded, run through SSRF guard
        // so /etc/hosts or resolver poisoning can't redirect bot-token POSTs
        // at LAN. The check is now inside the worker (was on producer thread).
        auto v = vms::utils::isInternalUrl(url);
        if (v.internal) {
            LOG_ERROR("[alert-delivery] Telegram URL refused by SSRF guard: host='{}' resolved='{}' reason={}",
                      v.host, v.resolved_ip, v.reason);
            return;
        }

        nlohmann::json payload;
        payload["chat_id"]    = chat_id;
        payload["text"]       = text;
        payload["parse_mode"] = "Markdown";

        CURL* curl = curl_easy_init();
        if (!curl) return;

        struct curl_slist* headers = nullptr;
        struct curl_slist* resolve_list = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        const std::string body = payload.dump();
        const std::string resolve_entry = vms::utils::buildResolveEntry(v);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
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
            LOG_ERROR("[alert-delivery] Telegram POST failed: curl={}", curl_easy_strerror(res));
        } else if (http_code >= 400) {
            LOG_WARN("[alert-delivery] Telegram POST returned http={}", http_code);
        } else {
            LOG_INFO("[alert-delivery] Telegram delivered to chat {} (rule '{}')", chat_id, rule_name);
        }

        curl_slist_free_all(headers);
        if (resolve_list) curl_slist_free_all(resolve_list);
        curl_easy_cleanup(curl);
    })) {
        LOG_THROTTLED_WARN(5000, "[alert-delivery] Telegram queue full, dropping send for rule '{}'", rule_name);
    }
}

void deliverAlarmOutput(const CompositeRule& rule, const RuleAction& /*action*/, const RawEvent& event) {
    // alarm_output_url setting read inside the worker (2026-05-15 audit) —
    // DbManager::getSetting is a real SELECT and the producer thread shouldn't
    // pay it. URL validation (http/https prefix) also moves into worker since
    // we'd otherwise need a second DB read on the producer just to validate.
    nlohmann::json payload = {
        {"action",     "alarm_trigger"},
        {"event_id",   event.event_id},
        {"event_type", eventTypeToString(event.type)},
        {"severity",   eventSeverityToString(event.severity)},
        {"camera_id",  event.camera_id},
        {"rule_id",    rule.rule_id},
        {"rule_name",  rule.name}
    };

    const std::string rule_name = rule.name;
    const std::uint64_t event_id = event.event_id;

    if (!alarmRunner().submit([payload_str = payload.dump(), rule_name, event_id]() {
        if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

        auto& db = vms::database::DbManager::getInstance();
        const std::string url = db.getSetting("alarm_output_url", "");
        if (url.empty()) {
            LOG_WARN("[alert-delivery] alarm_output_url not configured; alarm trigger logged only "
                     "(rule '{}', event {})", rule_name, event_id);
            return;
        }

        const bool is_http  = url.rfind("http://", 0) == 0;
        const bool is_https = url.rfind("https://", 0) == 0;
        if (!is_http && !is_https) {
            LOG_ERROR("[alert-delivery] alarm_output_url must be http(s)://. Got: {}", url);
            return;
        }

        // ALARM_OUTPUT typically points at a LAN relay — SSRF guard would
        // refuse private destinations, which is exactly what we expect here.
        // No SSRF check on this path (ssrf_required=false equivalent inline).
        CURL* curl = curl_easy_init();
        if (!curl) return;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (res != CURLE_OK) {
            LOG_ERROR("[alert-delivery] alarm POST {} failed: curl={}", url, curl_easy_strerror(res));
        } else if (http_code >= 400) {
            LOG_WARN("[alert-delivery] alarm POST {} returned http={}", url, http_code);
        } else {
            LOG_INFO("[alert-delivery] Alarm output triggered for event {} → {}", event_id, url);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    })) {
        LOG_THROTTLED_WARN(5000, "[alert-delivery] alarm-output queue full, dropping for rule '{}'", rule_name);
    }
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

namespace {
nlohmann::json runnerStatsJson(const vms::utils::BackgroundJobRunnerStats& s) {
    return nlohmann::json{
        {"name",                 s.name},
        {"worker_count",         s.worker_count},
        {"max_queue_size",       s.max_queue_size},
        {"current_queue_depth",  s.current_queue_depth},
        {"submitted_total",      s.submitted_total},
        {"dropped_total",        s.dropped_total},
        {"peak_queue_depth",     s.peak_queue_depth},
        {"stopping",             s.stopping}
    };
}
} // namespace

// Returns one block per per-channel pool plus an `aggregate` roll-up so
// pre-split dashboards still see one top-level submitted/dropped/peak triple.
// Schema change vs pre-2026-05-17: was flat (single pool's fields at top
// level), now nested. The `aggregate` block is permanent — operators want a
// single "total alerts lost" number alongside the per-channel breakdown.
nlohmann::json deliveryStats() {
    const auto webhook  = webhookRunner().stats();
    const auto sms      = smsRunner().stats();
    const auto telegram = telegramRunner().stats();
    const auto alarm    = alarmRunner().stats();

    // Peak is monotonic per pool; aggregate peak takes the max across pools
    // (not the sum) because the four queues are independent — a single
    // event cannot occupy a slot in more than one pool at the same time
    // for a given channel, and we want "what's the deepest any one queue
    // got" rather than "what's the sum of high-watermarks".
    const std::uint64_t agg_submitted = webhook.submitted_total + sms.submitted_total +
                                        telegram.submitted_total + alarm.submitted_total;
    const std::uint64_t agg_dropped   = webhook.dropped_total + sms.dropped_total +
                                        telegram.dropped_total + alarm.dropped_total;
    const std::uint64_t agg_peak = std::max({webhook.peak_queue_depth, sms.peak_queue_depth,
                                             telegram.peak_queue_depth, alarm.peak_queue_depth});

    return nlohmann::json{
        {"webhook",  runnerStatsJson(webhook)},
        {"sms",      runnerStatsJson(sms)},
        {"telegram", runnerStatsJson(telegram)},
        {"alarm",    runnerStatsJson(alarm)},
        {"aggregate", {
            {"submitted_total",  agg_submitted},
            {"dropped_total",    agg_dropped},
            {"peak_queue_depth", agg_peak}
        }}
    };
}

// Independent shutdowns — pools share no state. Order doesn't matter; each
// call is blocking until its workers join. Total wall time ≈ max(per-pool
// drain), not sum, because workers run in parallel even though the
// shutdown() calls themselves are sequential.
void shutdownDelivery() {
    webhookRunner().shutdown();
    smsRunner().shutdown();
    telegramRunner().shutdown();
    alarmRunner().shutdown();
}

} // namespace vms::events
