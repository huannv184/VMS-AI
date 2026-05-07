#pragma once

#include "core/user.h"
#include "database/db_manager.h"
#include <optional>
#include <vector>
#include <string>

namespace vms {
namespace database {

class UserRepository {
public:
    UserRepository() = default;
    ~UserRepository() = default;

    // User Operations
    std::optional<vms::core::User> getUserById(int id);
    std::optional<vms::core::User> getUserByUsername(const std::string& username);
    std::vector<vms::core::User> getAllUsers();
    bool createUser(const vms::core::User& user);
    bool updateUser(const vms::core::User& user);
    bool deleteUser(int id);
    bool updatePassword(int id, const std::string& password_hash);
    bool updatePasswordWithSalt(int id, const std::string& password_hash, const std::string& salt);
    bool updateLastLogin(int id);
    bool verifyCredentials(const std::string& username, const std::string& password_hash);
    bool update2FA(int id, bool enabled, const std::string& secret);
    // Increment token_version so all existing JWTs for this user are immediately rejected.
    bool bumpTokenVersion(int id);

    // Role Operations
    std::optional<vms::core::Role> getRoleById(int id);
    std::vector<vms::core::Role> getAllRoles();
};

} // namespace database
} // namespace vms
