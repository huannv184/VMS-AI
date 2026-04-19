#pragma once

#include "database/models.h"
#include <string>
#include <vector>
#include <mutex>
#include <memory>

namespace vms {
namespace core {

/**
 * @brief Manages alert rules and notification dispatching (M6)
 */
class AlertManager {
public:
    static AlertManager& getInstance();

    /**
     * @brief Process a new event against active alert rules
     */
    void processEvent(const vms::Event& event);

    /**
     * @brief Reload rules from database
     */
    void refreshRules();

private:
    AlertManager();
    ~AlertManager() = default;

    void dispatchNotification(const vms::Event& event, const std::string& action_type, const std::string& recipient);
    void sendEmail(const vms::Event& event, const std::string& recipient);
    void sendWebhook(const vms::Event& event, const std::string& url);

    struct AlertRule {
        int camera_id;
        std::string event_type;
        std::string action_type;
        std::string recipient;
    };

    std::vector<AlertRule> rules_;
    std::mutex rules_mutex_;
};

} // namespace core
} // namespace vms
