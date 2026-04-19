#include "core/alert_manager.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QSqlQuery>
#include <QSqlError>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

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
    // Basic implementation: Log to console as placeholder for SMTP logic
    LOG_INFO(">>> [MOCK EMAIL] To: {}, Subject: VMS Alert - {}, Body: {}", 
             recipient, event.event_type, event.description);
}

void AlertManager::sendWebhook(const vms::Event& event, const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return;

    nlohmann::json payload;
    payload["event_id"] = event.id;
    payload["camera_id"] = event.camera_id;
    payload["type"] = event.event_type;
    payload["message"] = event.description;
    payload["timestamp"] = event.timestamp;
    
    std::string json_str = payload.dump();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG_ERROR("AlertManager: Webhook failed: {}", curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

} // namespace core
} // namespace vms
