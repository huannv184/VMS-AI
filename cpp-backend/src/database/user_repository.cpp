#include "database/user_repository.h"
#include "utils/logger.h"
#include "utils/crypto.h"
#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>
#include <nlohmann/json.hpp>
#include <ctime>

namespace vms {
namespace database {

std::optional<vms::core::User> UserRepository::getUserByUsername(const std::string& username) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT u.id, u.username, u.password_hash, u.salt, u.role_id, u.full_name, u.email, "
                  "u.is_active, u.last_login, u.created_at, u.two_factor_enabled, u.two_factor_secret, r.name, r.permissions, "
                  "COALESCE(u.token_version, 0) "
                  "FROM users u "
                  "LEFT JOIN roles r ON u.role_id = r.id "
                  "WHERE u.username = ?";

    QSqlQuery query(db);
    if (!query.prepare(sql)) {
        LOG_ERROR("Failed to prepare getUserByUsername: {}", query.lastError().text().toStdString());
        return std::nullopt;
    }
    query.bindValue(0, QString::fromStdString(username));

    if (!query.exec()) {
        LOG_ERROR("Failed to execute getUserByUsername: {}", query.lastError().text().toStdString());
        return std::nullopt;
    }

    if (query.next()) {
        vms::core::User user;
        user.id = query.value(0).toInt();
        user.username = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        user.password_hash = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        user.salt = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        user.role_id = query.value(4).toInt();
        user.full_name = query.value(5).isNull() ? "" : query.value(5).toString().toStdString();
        user.email = query.value(6).isNull() ? "" : query.value(6).toString().toStdString();
        user.is_active = query.value(7).toInt();
        user.last_login = query.value(8).toLongLong();
        user.created_at = query.value(9).toLongLong();
        user.two_factor_enabled = query.value(10).toInt() != 0;

        // [MUST-FIX] 2FA Leak: Decrypt secret at rest
        std::string encrypted_secret = query.value(11).isNull() ? "" : query.value(11).toString().toStdString();
        user.two_factor_secret = vms::utils::Crypto::decrypt(encrypted_secret);

        user.role_name = query.value(12).isNull() ? "unknown" : query.value(12).toString().toStdString();       
        QString perms_json = query.value(13).toString();
        if (!perms_json.isEmpty()) {
            try {
                auto j = nlohmann::json::parse(perms_json.toStdString());
                user.permissions = j.get<std::vector<std::string>>();
            } catch (...) {
                user.permissions = {};
            }
        }
        user.token_version = query.value(14).toInt();

        return user;
    }

    return std::nullopt;
}

std::optional<vms::core::User> UserRepository::getUserById(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT u.id, u.username, u.password_hash, u.salt, u.role_id, u.full_name, u.email, "
                  "u.is_active, u.last_login, u.created_at, u.two_factor_enabled, u.two_factor_secret, r.name, r.permissions, "
                  "COALESCE(u.token_version, 0) "
                  "FROM users u "
                  "LEFT JOIN roles r ON u.role_id = r.id "
                  "WHERE u.id = ?";

    QSqlQuery query(db);
    if (!query.prepare(sql)) {
        LOG_ERROR("Failed to prepare getUserById: {}", query.lastError().text().toStdString());
        return std::nullopt;
    }
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute getUserById: {}", query.lastError().text().toStdString());
        return std::nullopt;
    }

    if (query.next()) {
        vms::core::User user;
        user.id = query.value(0).toInt();
        user.username = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        user.password_hash = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        user.salt = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        user.role_id = query.value(4).toInt();
        user.full_name = query.value(5).isNull() ? "" : query.value(5).toString().toStdString();
        user.email = query.value(6).isNull() ? "" : query.value(6).toString().toStdString();
        user.is_active = query.value(7).toInt();
        user.last_login = query.value(8).toLongLong();
        user.created_at = query.value(9).toLongLong();
        user.two_factor_enabled = query.value(10).toInt() != 0;

        // [MUST-FIX] 2FA Leak: Decrypt secret at rest
        std::string encrypted_secret = query.value(11).isNull() ? "" : query.value(11).toString().toStdString();
        user.two_factor_secret = vms::utils::Crypto::decrypt(encrypted_secret);

        user.role_name = query.value(12).isNull() ? "unknown" : query.value(12).toString().toStdString();       
        QString perms_json = query.value(13).toString();
        if (!perms_json.isEmpty()) {
            try {
                auto j = nlohmann::json::parse(perms_json.toStdString());
                user.permissions = j.get<std::vector<std::string>>();
            } catch (...) {
                user.permissions = {};
            }
        }
        user.token_version = query.value(14).toInt();

        return user;
    }

    return std::nullopt;
}

bool UserRepository::createUser(const vms::core::User& user) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "INSERT INTO users (username, password_hash, salt, role_id, full_name, email, is_active, created_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, QString::fromStdString(user.username));
    query.bindValue(1, QString::fromStdString(user.password_hash));
    query.bindValue(2, QString::fromStdString(user.salt));
    query.bindValue(3, user.role_id);
    query.bindValue(4, QString::fromStdString(user.full_name));
    query.bindValue(5, QString::fromStdString(user.email));
    query.bindValue(6, user.is_active ? 1 : 0);
    query.bindValue(7, static_cast<qint64>(std::time(nullptr)));

    if (!query.exec()) {
        LOG_ERROR("Failed to create user: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

bool UserRepository::verifyCredentials(const std::string& username, const std::string& password_hash) {
    // Note: In real world, use bcrypt verify. Here we compare pre-hashed values from client/service
    auto user = getUserByUsername(username);
    if (!user) return false;
    if (!user->is_active) return false;

    // Constant-time comparison to prevent timing attacks
    const std::string& a = user->password_hash;
    const std::string& b = password_hash;

    if (a.length() != b.length()) {
        return false;
    }

    volatile unsigned char result = 0;
    for (size_t i = 0; i < a.length(); i++) {
        result |= (a[i] ^ b[i]);
    }

    return result == 0;
}

std::vector<vms::core::User> UserRepository::getAllUsers() {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT u.id, u.username, u.role_id, u.full_name, u.email, u.is_active, "
                  "u.last_login, u.created_at, r.name, r.permissions "
                  "FROM users u LEFT JOIN roles r ON u.role_id = r.id";

    std::vector<vms::core::User> users;

    QSqlQuery query(db);
    query.prepare(sql);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute getAllUsers: {}", query.lastError().text().toStdString());
        return users;
    }

    while (query.next()) {
        vms::core::User user;
        user.id = query.value(0).toInt();
        user.username = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        user.role_id = query.value(2).toInt();
        user.full_name = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        user.email = query.value(4).isNull() ? "" : query.value(4).toString().toStdString();
        user.is_active = query.value(5).toInt();
        user.last_login = query.value(6).toLongLong();
        user.created_at = query.value(7).toLongLong();
        user.role_name = query.value(8).isNull() ? "unknown" : query.value(8).toString().toStdString();

        QString perms_json = query.value(9).toString();
        if (!perms_json.isEmpty()) {
            try {
                auto j = nlohmann::json::parse(perms_json.toStdString());
                user.permissions = j.get<std::vector<std::string>>();
            } catch (...) {
                user.permissions = {};
            }
        }

        users.push_back(user);
    }

    return users;
}

bool UserRepository::updateUser(const vms::core::User& user) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "UPDATE users SET full_name = ?, email = ?, role_id = ?, is_active = ? WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, QString::fromStdString(user.full_name));
    query.bindValue(1, QString::fromStdString(user.email));
    query.bindValue(2, user.role_id);
    query.bindValue(3, user.is_active ? 1 : 0);
    query.bindValue(4, user.id);

    if (!query.exec()) {
        LOG_ERROR("Failed to update user: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

bool UserRepository::deleteUser(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "DELETE FROM users WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to delete user: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

bool UserRepository::updatePassword(int id, const std::string& password_hash) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "UPDATE users SET password_hash = ? WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, QString::fromStdString(password_hash));
    query.bindValue(1, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to update password: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

bool UserRepository::updatePasswordWithSalt(int id, const std::string& password_hash, const std::string& salt) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "UPDATE users SET password_hash = ?, salt = ? WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, QString::fromStdString(password_hash));
    query.bindValue(1, QString::fromStdString(salt));
    query.bindValue(2, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to update password with salt: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

std::optional<vms::core::Role> UserRepository::getRoleById(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, name, permissions, created_at FROM roles WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute getRoleById: {}", query.lastError().text().toStdString());
        return std::nullopt;
    }

    if (query.next()) {
        vms::core::Role role;
        role.id = query.value(0).toInt();
        role.name = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();

        QString perms_json = query.value(2).toString();
        if (!perms_json.isEmpty()) {
            try {
                auto j = nlohmann::json::parse(perms_json.toStdString());
                role.permissions = j.get<std::vector<std::string>>();
            } catch (...) {
                role.permissions = {};
            }
        }

        role.created_at = query.value(3).toLongLong();
        return role;
    }

    return std::nullopt;
}

std::vector<vms::core::Role> UserRepository::getAllRoles() {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, name, permissions, created_at FROM roles";

    std::vector<vms::core::Role> roles;

    QSqlQuery query(db);
    query.prepare(sql);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute getAllRoles: {}", query.lastError().text().toStdString());
        return roles;
    }

    while (query.next()) {
        vms::core::Role role;
        role.id = query.value(0).toInt();
        role.name = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();

        QString perms_json = query.value(2).toString();
        if (!perms_json.isEmpty()) {
            try {
                auto j = nlohmann::json::parse(perms_json.toStdString());
                role.permissions = j.get<std::vector<std::string>>();
            } catch (...) {
                role.permissions = {};
            }
        }

        role.created_at = query.value(3).toLongLong();
        roles.push_back(role);
    }

    return roles;
}

bool UserRepository::updateLastLogin(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "UPDATE users SET last_login = ? WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, static_cast<qint64>(std::time(nullptr)));
    query.bindValue(1, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to update last login: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

bool UserRepository::update2FA(int id, bool enabled, const std::string& secret) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    // [MUST-FIX] 2FA Leak: Encrypt secret at rest
    std::string encrypted_secret = vms::utils::Crypto::encrypt(secret);

    QString sql = "UPDATE users SET two_factor_enabled = ?, two_factor_secret = ? WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, enabled ? 1 : 0);
    query.bindValue(1, QString::fromStdString(encrypted_secret));
    query.bindValue(2, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to update 2FA: {}", query.lastError().text().toStdString());
        return false;
    }

    return true;
}

bool UserRepository::bumpTokenVersion(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    QSqlQuery query(db);
    query.prepare("UPDATE users SET token_version = token_version + 1 WHERE id = ?");
    query.bindValue(0, id);
    if (!query.exec()) {
        LOG_ERROR("Failed to bump token_version for user {}: {}", id, query.lastError().text().toStdString());
        return false;
    }
    return true;
}

} // namespace database
} // namespace vms
