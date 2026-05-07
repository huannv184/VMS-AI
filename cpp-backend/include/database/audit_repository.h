#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vms {
namespace database {

struct AuditLog {
    int id;
    int user_id;
    std::string action;
    std::string details;
    long long created_at;

    nlohmann::json toJson() const {
        return {
            {"id", id},
            {"user_id", user_id},
            {"action", action},
            {"details", details},
            {"created_at", created_at}
        };
    }
};

class AuditRepository {
public:
    AuditRepository() = default;

    bool insertLog(int user_id, const std::string& action, const std::string& details);
    std::vector<AuditLog> getLogs(int limit = 50, int offset = 0);
};

} // namespace database
} // namespace vms
