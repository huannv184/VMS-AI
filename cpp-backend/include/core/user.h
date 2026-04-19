#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vms {
namespace core {

struct Role {
    int id;
    std::string name;
    std::vector<std::string> permissions;
    int64_t created_at;

    nlohmann::json toJson() const {
        return {
            {"id", id},
            {"name", name},
            {"permissions", permissions},
            {"created_at", created_at}
        };
    }
};

struct User {
    int id;
    std::string username;
    std::string password_hash;
    std::string salt; // Per-user random salt
    int role_id;
    std::string full_name;
    std::string email;
    bool is_active;
    int64_t last_login;
    int64_t created_at;
    bool two_factor_enabled = false;
    std::string two_factor_secret;
    
    // Token version: incremented when user is deactivated, role changes, or forced logout.
    // JWT claim "ver" must match this value; mismatch rejects the token immediately.
    int token_version = 0;

    // Aggregated Role (fetched via JOIN usually)
    std::string role_name;
    std::vector<std::string> permissions;

    nlohmann::json toJson(bool include_private = false) const {
        nlohmann::json j = {
            {"id", id},
            {"username", username},
            {"role_id", role_id},
            {"role_name", role_name},
            {"full_name", full_name},
            {"email", email},
            {"is_active", is_active},
            {"last_login", last_login},
            {"created_at", created_at},
            {"permissions", permissions},
            {"two_factor_enabled", two_factor_enabled}
        };
        // SEC-006: Never expose sensitive fields via API.
        // password_hash and two_factor_secret are internal-only.
        (void)include_private; // Suppress unused parameter warning
        return j;
    }
};

} // namespace core
} // namespace vms
