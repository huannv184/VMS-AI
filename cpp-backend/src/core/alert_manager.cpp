#include "core/alert_manager.h"
#include "core/runtime_state.h"
#include "database/db_manager.h"
#include "utils/background_job_runner.h"
#include "utils/email_sender.h"
#include "utils/logger.h"
#include <QSqlQuery>
#include <QSqlError>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace {

// Webhooks used to run inline on EventManager's broadcast loop — one slow
// endpoint stalled the entire event pipeline. Move them to a bounded
// background runner (drop-on-full) mirroring the AlertRouter pattern.
vms::utils::BackgroundJobRunner& webhookRunner() {
    static vms::utils::BackgroundJobRunner instance("alert-mgr-webhooks", 2, 128);
    return instance;
}

} // namespace

namespace vms {
namespace core {

AlertManager& AlertManager::getInstance() {
    static AlertManager instance;
    return instance;
}

AlertManager::AlertManager() {
    refreshRules();
}

void AlertManager::refreshRules() {
    std::lock_guard<std::mutex> lock(rules_mutex_);
    rules_.clear();

    QSqlDatabase db = database::DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return;

    QSqlQuery query(db);
    query.prepare("SELECT camera_id, event_type, action_type, recipient FROM alert_rules WHERE is_enabled = 1");
    
    if (query.exec()) {
        while (query.next()) {
            AlertRule rule;
            rule.camera_id = query.value(0).toInt();
            rule.event_type = query.value(1).toString().toStdString();
            rule.action_type = query.value(2).toString().toStdString();
            rule.recipient = query.value(3).toString().toStdString();
            rules_.push_back(rule);
        }
    }
    LOG_INFO("AlertManager: Loaded {} active rules", rules_.size());
}

void AlertManager::processEvent(const vms::Event& event) {
    std::lock_guard<std::mutex> lock(rules_mutex_);
    for (const auto& rule : rules_) {
        bool camera_match = (rule.camera_id == -1 || rule.camera_id == event.camera_id);
        bool type_match = (rule.event_type == "*" || rule.event_type == event.event_type);

        if (camera_match && type_match) {
            dispatchNotification(event, rule.action_type, rule.recipient);
        }
    }
}

void AlertManager::dispatchNotification(const vms::Event& event, const std::string& action_type, const std::string& recipient) {
    LOG_INFO("AlertManager: Dispatching {} to {}", action_type, recipient);
    
    if (action_type == "webhook") {
        sendWebhook(event, recipient);
    } else if (action_type == "email") {
        sendEmail(event, recipient);
    } else {
        LOG_WARN("AlertManager: Unknown action type {}", action_type);
    }
}

void AlertManager::sendEmail(const vms::Event& event, const std::string& recipient) {
    // Subject must be single-line (sendEmailAsync sanitizes again, but we keep
    // the human-readable form short here so logs aren't noisy).
    std::string subject = "VMS Alert — ";
    subject += event.event_type;
    if (event.camera_id > 0) {
        subject += " (camera " + std::to_string(event.camera_id) + ")";
    }

    std::ostringstream body;
    body << "VMS Alert\r\n\r\n"
         << "Event type: " << event.event_type << "\r\n"
         << "Camera:     " << event.camera_id << "\r\n"
         << "Timestamp:  " << event.timestamp << "\r\n"
         << "Event ID:   " << event.id << "\r\n\r\n"
         << "Description:\r\n" << event.description << "\r\n";

    vms::utils::sendEmailAsync(subject, body.str(), { recipient }, "alert_manager");
}

void AlertManager::sendWebhook(const vms::Event& event, const std::string& url) {
    // Build the payload on the calling thread (EventManager broadcast loop)
    // so a stale `event` reference can't outlive the queued job. Then hand
    // the cheap value-type copy to the background runner — the actual
    // libcurl perform must NEVER block the event loop.
    nlohmann::json payload;
    payload["event_id"] = event.id;
    payload["camera_id"] = event.camera_id;
    payload["type"] = event.event_type;
    payload["message"] = event.description;
    payload["timestamp"] = event.timestamp;
    std::string json_str = payload.dump();
    std::string url_copy = url;

    if (!webhookRunner().submit([url_copy, json_str]() {
        if (vms::core::shutting_down.load(std::memory_order_acquire)) return;

        CURL* curl = curl_easy_init();
        if (!curl) return;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url_copy.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        // Without these, a dead webhook host blocks a worker for the OS
        // default ~120s. Two workers + 60 dead hosts = full stall.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            LOG_ERROR("AlertManager: Webhook to {} failed: {}", url_copy, curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    })) {
        LOG_THROTTLED_WARN(5000, "AlertManager: webhook queue full, dropping send to {}", url_copy);
    }
}

} // namespace core
} // namespace vms
