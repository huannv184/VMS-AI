#include "api/user_controller.h"
#include "utils/api_utils.h"
#include "utils/config.h"
#include "utils/logger.h"
#include "utils/sha256.h"
#include "utils/jwt_utils.h"
#include "middleware/ldap_connector.h"
#include "database/audit_repository.h"
#include "database/permission_repository.h"
#include "database/db_state.h"
#include "middleware/auth_middleware.h"
#include "utils/totp.h"
#include "utils/validator.h"
#include "utils/rate_limiter.h"
#include "utils/password_hash.h"
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

using json = nlohmann::json;

namespace vms {
namespace api {

static std::string generateSaltHex(size_t bytes = 16) {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 255);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes; i++) {
        oss << std::setw(2) << dist(rd);
    }
    return oss.str();
}

// H6: New passwords use argon2id (VMS_HAS_ARGON2 compile flag required).
// Legacy SHA256 hashes are still accepted on login for backward compatibility
// and automatically re-hashed on successful login.
// 
// ALWAYS use this for new password creation/changes.
static std::string hashPasswordNew(const std::string& password) {
#ifdef VMS_HAS_ARGON2
    const std::string salt = vms::utils::generateArgon2Salt();
    return vms::utils::hashPasswordArgon2(password, salt);
#else
    // Fallback: SHA256+salt until argon2 is added to the build
    // NOTE: This fallback format matches the migration logic in login.
    const std::string salt = generateSaltHex();
    return vms::utils::SHA256::hash(password + salt) + ":" + salt;
#endif
}

// Legacy SHA256 verifier — used ONLY during migration on login.
// DO NOT use this for creating new hashes.
static std::string hashPasswordV2(const std::string& password, const std::string& username, const std::string& salt) {
    return vms::utils::SHA256::hash(password + username + salt);
}

static bool shouldUseSecureCookie(const crow::request& req) {
    const std::string forwarded_proto = req.get_header_value("X-Forwarded-Proto");
    if (!forwarded_proto.empty()) {
        return forwarded_proto == "https";
    }

    const std::string origin = req.get_header_value("Origin");
    return origin.rfind("https://", 0) == 0;
}

static json buildAuthResponse(bool requires_2fa,
                              const std::optional<vms::core::User>& user,
                              const std::string& token = "",
                              const std::string& temp_token = "",
                              bool password_change_required = false) {
    json data = {
        {"token", token.empty() ? json(nullptr) : json(token)},
        {"temp_token", temp_token.empty() ? json(nullptr) : json(temp_token)},
        {"user", user.has_value() ? user->toJson() : json(nullptr)},
        {"requires_2fa", requires_2fa},
        {"password_change_required", password_change_required}
    };

    return {
        {"success", true},
        {"data", data},
        {"error", nullptr},
        {"meta", ApiUtils::makeMeta()}
    };
}

void UserController::registerRoutes(vms::server::VmsApp& app) {
    
    // POST /api/auth/login
    CROW_ROUTE(app, "/api/auth/login")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            auto body = json::parse(req.body);
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");

            if (username.empty() || password.empty()) {
                return ApiUtils::createErrorResponse("Username and password are required", 400, origin);
            }
            if (!vms::Validator::isSafeCredential(username)) {
                return ApiUtils::createErrorResponse("Invalid username format", 400, origin);
            }
            if (password.size() > 256) {
                return ApiUtils::createErrorResponse("Password too long", 400, origin);
            }

            // H4: Rate limiting — per-IP (5 failures/60s) and per-username (10 failures/300s)
            const std::string client_ip = req.get_header_value("X-Forwarded-For").empty()
                ? req.remote_ip_address
                : req.get_header_value("X-Forwarded-For").substr(
                      0, req.get_header_value("X-Forwarded-For").find(','));
            if (auto msg = vms::utils::RateLimiter::getInstance().checkLogin(client_ip, username); !msg.empty()) {
                return ApiUtils::createErrorResponse(msg, 429, origin);
            }
            
            database::UserRepository repo;
            auto user_opt = repo.getUserByUsername(username);
            bool local_auth_success = false;

            if (user_opt) {
                // H6: Verify password. Check argon2id first (new format), then legacy SHA256.
                bool argon2_match = vms::utils::isArgon2Hash(user_opt->password_hash) &&
                                    vms::utils::verifyPasswordArgon2(password, user_opt->password_hash);

                bool legacy_match = false;
                if (!argon2_match && !vms::utils::isArgon2Hash(user_opt->password_hash)) {
                    // Legacy formats: per-user salt, global salt, unsalted
                    std::string v2_hash = (!user_opt->salt.empty())
                        ? hashPasswordV2(password, user_opt->username, user_opt->salt)
                        : std::string{};
                    std::string global_salt_hash = vms::utils::SHA256::hash(
                        password + user_opt->username + "VMS_GLOBAL_SALT");
                    std::string bare_hash = vms::utils::SHA256::hash(password);

                    legacy_match = (!v2_hash.empty() && v2_hash == user_opt->password_hash) ||
                                   global_salt_hash == user_opt->password_hash ||
                                   bare_hash == user_opt->password_hash;
                }

                if (argon2_match || legacy_match) {
                    local_auth_success = true;

                    // H6: Auto-migrate legacy SHA256 hashes to argon2id on successful login.
                    if (legacy_match) {
#ifdef VMS_HAS_ARGON2
                        try {
                            const std::string new_hash = hashPasswordNew(password);
                            repo.updatePasswordWithSalt(user_opt->id, new_hash, "argon2");
                            user_opt->password_hash = new_hash;
                            LOG_INFO("[Auth] Migrated password for user '{}' to argon2id", user_opt->username);
                        } catch (const std::exception& e) {
                            LOG_WARN("[Auth] Failed to migrate password hash for '{}': {}", user_opt->username, e.what());
                        }
#else
                        // Without argon2, still upgrade from bare/global-salt to per-user SHA256
                        const std::string new_salt = generateSaltHex();
                        const std::string new_hash = hashPasswordV2(password, user_opt->username, new_salt);
                        repo.updatePasswordWithSalt(user_opt->id, new_hash, new_salt);
                        user_opt->salt = new_salt;
                        user_opt->password_hash = new_hash;
#endif
                    }
                }
            }

            if (local_auth_success) {
                vms::utils::RateLimiter::getInstance().recordLoginSuccess(client_ip, username);
                // SEC-001: Check if this user is logging in with the default password.
                // If so, require an immediate password change before allowing full access.
                bool must_change_password = false;
                if (user_opt->username == "admin" &&
                    vms::database::default_password_active.load(std::memory_order_acquire)) {
                    must_change_password = true;
                    LOG_CRITICAL("SEC-001: Admin login with default password detected — password change required");
                }

                if (user_opt->two_factor_enabled) {
                    return ApiUtils::createResponse(
                        buildAuthResponse(
                            true,
                            user_opt,
                            "",
                            vms::utils::createTwoFactorTempTokenJwt(*user_opt),
                            must_change_password
                        ),
                        200,
                        origin
                    );
                }

                repo.updateLastLogin(user_opt->id);
                std::string token = vms::utils::createAccessTokenJwt(*user_opt);

                database::AuditRepository audit;
                audit.insertLog(user_opt->id, "LOGIN", "Local login successful");

                // H5: Set HttpOnly Secure SameSite=Strict cookie so browsers never
                // expose the token to JavaScript (mitigates XSS token theft).
                // The JSON response still includes the token for native/mobile clients
                // that cannot use cookies.
                const int ttl_sec = vms::Config::getInstance().getAuthConfig().token_expire_minutes * 60;
                std::string cookie_val = "vms_session=" + token +
                    "; HttpOnly; SameSite=Strict; Path=/api; Max-Age=" + std::to_string(ttl_sec);
                if (shouldUseSecureCookie(req)) {
                    cookie_val += "; Secure";
                }

                auto resp = ApiUtils::createResponse(
                    buildAuthResponse(false, user_opt, token, "", must_change_password),
                    200, origin);
                resp.set_header("Set-Cookie", cookie_val);
                return resp;
            }

            // LDAP Fallback
            auto& config = vms::Config::getInstance().getLdapConfig();
            if (config.isEnabled) {
                vms::middleware::LdapConnector ldap(config.ldapHost, config.ldapPort, config.ldapDomain);
                if (ldap.authenticate(username, password)) {
                    if (!user_opt) {
                        vms::core::User newUser;
                        newUser.username = username;
                        newUser.role_id = 2; // Default to user
                        newUser.is_active = true;
                        repo.createUser(newUser);
                        user_opt = repo.getUserByUsername(username);
                    }
                    if (user_opt) {
                        repo.updateLastLogin(user_opt->id);
                        std::string token = vms::utils::createAccessTokenJwt(*user_opt);
                        return ApiUtils::createResponse(buildAuthResponse(false, user_opt, token), 200, origin);
                    }
                }
            }

            vms::utils::RateLimiter::getInstance().recordLoginFailure(client_ip, username);
            return ApiUtils::createErrorResponse("Invalid credentials", 401, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // GET /api/auth/me
    CROW_ROUTE(app, "/api/auth/me")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
            if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
            return ApiUtils::createResponse({{"user", ctx.user->toJson()}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // POST /api/ws/ticket
    CROW_ROUTE(app, "/api/ws/ticket")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
            if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);
            
            std::string client_ip = req.get_header_value("X-Forwarded-For").empty()
                ? req.remote_ip_address
                : req.get_header_value("X-Forwarded-For").substr(
                      0, req.get_header_value("X-Forwarded-For").find(','));

            // Generate a 30-second single-use stateless JWT ticket bound to IP
            std::string ticket = vms::utils::createWsTicketJwt(*ctx.user, client_ip);
            
            return ApiUtils::createResponse({
                {"ticket", ticket},
                {"expires_in", 30}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });

    // POST /api/auth/logout
    CROW_ROUTE(app, "/api/auth/logout")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        return ApiUtils::createResponse(json::object(), 200, origin);
    });

    // /api/users
    CROW_ROUTE(app, "/api/users")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) {
            return ApiUtils::createErrorResponse("Admin privileges required", 403, origin);
        }

        database::UserRepository repo;
        if (req.method == crow::HTTPMethod::Get) {
            auto users = repo.getAllUsers();
            json j = json::array();
            for (const auto& u : users) j.push_back(u.toJson());
            return ApiUtils::createResponse({{"users", j}}, 200, origin);
        }

        if (req.method == crow::HTTPMethod::Post) {
            try {
                auto body = json::parse(req.body);
                vms::core::User user;
                user.username = body.value("username", "");
                std::string raw_pass = body.value("password", "");
                if (user.username.empty() || raw_pass.empty()) return ApiUtils::createErrorResponse("Username and password required", 400, origin);
                
                user.password_hash = hashPasswordNew(raw_pass);
#ifdef VMS_HAS_ARGON2
                user.salt = "argon2";
#else
                user.salt = "sha256";
#endif
                user.role_id = body.value("role_id", 2);
                user.full_name = body.value("full_name", "");
                user.is_active = body.value("is_active", true);

                if (repo.createUser(user)) {
                    database::AuditRepository audit;
                    audit.insertLog(ctx.user->id, "CREATE_USER", "Created user: " + user.username);
                    return ApiUtils::createResponse(json::object(), 201, origin);
                }
            } catch (...) {}
            return ApiUtils::createErrorResponse("Failed to create user", 500, origin);
        }
        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // /api/users/<int>
    CROW_ROUTE(app, "/api/users/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) return ApiUtils::createErrorResponse("Admin privileges required", 403, origin);

        database::UserRepository repo;
        if (req.method == crow::HTTPMethod::Get) {
            auto u = repo.getUserById(id);
            if (!u) return ApiUtils::createErrorResponse("Not found", 404, origin);
            return ApiUtils::createResponse(u->toJson(), 200, origin);
        }

        if (req.method == crow::HTTPMethod::Put) {
            auto u_opt = repo.getUserById(id);
            if (!u_opt) return ApiUtils::createErrorResponse("Not found", 404, origin);
            auto body = json::parse(req.body);
            if (body.contains("full_name")) u_opt->full_name = body["full_name"];
            if (body.contains("role_id")) u_opt->role_id = body["role_id"];
            if (body.contains("is_active")) u_opt->is_active = body["is_active"];
            
            if (repo.updateUser(*u_opt)) {
                database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "UPDATE_USER", "Updated user ID: " + std::to_string(id));
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
        }

        if (req.method == crow::HTTPMethod::Delete) {
            if (repo.deleteUser(id)) {
                database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "DELETE_USER", "Deleted user ID: " + std::to_string(id));
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
        }
        return ApiUtils::createErrorResponse("Operation failed", 500, origin);
    });

    // POST /api/users/change-password
    CROW_ROUTE(app, "/api/users/change-password")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);

        try {
            auto body = json::parse(req.body);
            std::string old_pass = body.value("old_password", "");
            std::string new_pass = body.value("new_password", "");

            if (old_pass.empty() || new_pass.empty())
                return ApiUtils::createErrorResponse("old_password and new_password are required", 400, origin);
            if (new_pass.size() < 8 || new_pass.size() > 256)
                return ApiUtils::createErrorResponse("new_password must be 8-256 characters", 400, origin);

            database::UserRepository repo;
            auto user = repo.getUserById(ctx.user->id);
            if (!user.has_value()) return ApiUtils::createErrorResponse("User not found", 404, origin);

            std::string expected_old = "";
            if (!user->salt.empty()) {
                expected_old = hashPasswordV2(old_pass, user->username, user->salt);
            } else {
                expected_old = vms::utils::SHA256::hash(old_pass + user->username + "VMS_GLOBAL_SALT");
            }
            if (user->password_hash != expected_old) return ApiUtils::createErrorResponse("Invalid old password", 401, origin);

            // H6: Use argon2id for new password
            std::string new_hash = hashPasswordNew(new_pass);
            std::string new_salt;
#ifdef VMS_HAS_ARGON2
            new_salt = "argon2";
#else
            new_salt = "sha256";
#endif
            if (repo.updatePasswordWithSalt(user->id, new_hash, new_salt)) {
                // SEC-001: Clear default-password flag if admin just changed their password
                if (user->username == "admin" &&
                    vms::database::default_password_active.load(std::memory_order_acquire)) {
                    vms::database::default_password_active.store(false, std::memory_order_release);
                    LOG_INFO("SEC-001: Admin default password cleared — system secured");
                }
                database::AuditRepository audit;
                audit.insertLog(user->id, "CHANGE_PASSWORD", "Self password change");
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
        } catch (...) {}
        return ApiUtils::createErrorResponse("Failed", 500, origin);
    });

    // POST /api/users/<int>/reset-password
    CROW_ROUTE(app, "/api/users/<int>/reset-password")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) return ApiUtils::createErrorResponse("Admin required", 403, origin);

        try {
            auto body = json::parse(req.body);
            std::string new_pass = body.value("new_password", "");
            if (new_pass.size() < 8 || new_pass.size() > 256)
                return ApiUtils::createErrorResponse("new_password must be 8-256 characters", 400, origin);
            database::UserRepository repo;
            auto u = repo.getUserById(id);
            if (!u.has_value()) return ApiUtils::createErrorResponse("User not found", 404, origin);
            
            std::string h = hashPasswordNew(new_pass);
            std::string new_salt;
#ifdef VMS_HAS_ARGON2
            new_salt = "argon2";
#else
            new_salt = "sha256";
#endif
            if (repo.updatePasswordWithSalt(id, h, new_salt)) {
                // SEC-001: Clear default-password flag when admin account is reset
                if (u->username == "admin" &&
                    vms::database::default_password_active.load(std::memory_order_acquire)) {
                    vms::database::default_password_active.store(false, std::memory_order_release);
                    LOG_INFO("SEC-001: Admin default password cleared via reset — system secured");
                }
                database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "RESET_PASSWORD", "Admin reset password for: " + u->username);
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
        } catch (...) {}
        return ApiUtils::createErrorResponse("Failed", 500, origin);
    });

    // ============================================================================
    // PERMISSIONS API (RBAC)
    // ============================================================================

    // /api/permissions/<int>
    CROW_ROUTE(app, "/api/permissions/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value() || ctx.user->role_id != 1) return ApiUtils::createErrorResponse("Admin required", 403, origin);

        if (req.method == crow::HTTPMethod::Get) {
            auto p = database::PermissionRepository::getPermissions(id);
            json j = {
                {"user_id", p.user_id},
                {"allowed_camera_ids", p.allowed_camera_ids},
                {"allowed_site_ids", p.allowed_site_ids},
                {"can_view_live", p.can_view_live},
                {"can_view_playback", p.can_view_playback},
                {"can_manage_settings", p.can_manage_settings},
                {"can_ptz", p.can_ptz}
            };
            return ApiUtils::createResponse(j, 200, origin);
        }

        if (req.method == crow::HTTPMethod::Put) {
            try {
                auto body = json::parse(req.body);
                UserPermission p;
                p.user_id = id;
                p.allowed_camera_ids = body.value("allowed_camera_ids", std::vector<int>{});
                p.allowed_site_ids = body.value("allowed_site_ids", std::vector<int>{});
                p.can_view_live = body.value("can_view_live", true);
                p.can_view_playback = body.value("can_view_playback", true);
                p.can_manage_settings = body.value("can_manage_settings", false);
                p.can_ptz = body.value("can_ptz", true);

                if (database::PermissionRepository::setPermissions(p)) {
                    return ApiUtils::createResponse(json::object(), 200, origin);
                }
            } catch (...) {}
            return ApiUtils::createErrorResponse("Failed to update permissions", 500, origin);
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // ============================================================================
    // 2FA API
    // ============================================================================

    // POST /api/auth/2fa/setup
    CROW_ROUTE(app, "/api/auth/2fa/setup")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);

        std::string secret = vms::utils::TOTP::generateSecret();
        std::string uri = vms::utils::TOTP::getProvisioningUri(ctx.user->username, secret);
        
        return ApiUtils::createResponse({
            {"secret", secret},
            {"otpauth_url", uri}
        }, 200, origin);
    });

    // POST /api/auth/2fa/enable
    CROW_ROUTE(app, "/api/auth/2fa/enable")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (!ctx.user.has_value()) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);

        try {
            auto body = json::parse(req.body);
            std::string code = body.value("code", "");
            std::string secret = body.value("secret", "");
            if (!vms::Validator::isSafeCredential(code, 16) || !vms::Validator::isSafeCredential(secret, 128)) {
                return ApiUtils::createErrorResponse("Invalid 2FA payload", 400, origin);
            }
            if (code.empty() || secret.empty()) {
                return ApiUtils::createErrorResponse("code and secret are required", 400, origin);
            }

            if (vms::utils::TOTP::verifyCode(secret, code)) {
                database::UserRepository repo;
                if (repo.update2FA(ctx.user->id, true, secret)) {
                    database::AuditRepository audit;
                    audit.insertLog(ctx.user->id, "2FA_ENABLE", "2FA enabled");
                    return ApiUtils::createResponse({
                        {"success", true},
                        {"data", json::object()},
                        {"error", nullptr}
                    }, 200, origin);
                }
            }
        } catch (...) {}
        return ApiUtils::createErrorResponse("Invalid code or failed to enable", 400, origin);
    });

    // POST /api/auth/2fa/verify (During Login)
    CROW_ROUTE(app, "/api/auth/2fa/verify")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto body = json::parse(req.body);
            std::string code = body.value("code", "");
            std::string temp_token = body.value("temp_token", "");

            if (code.empty()) {
                return ApiUtils::createErrorResponse("code is required", 400, origin);
            }
            if (temp_token.empty()) {
                return ApiUtils::createErrorResponse("temp_token is required", 400, origin);
            }
            if (!vms::Validator::isSafeCredential(code, 16)) {
                return ApiUtils::createErrorResponse("Invalid 2FA code format", 400, origin);
            }

            database::UserRepository repo;
            int user_id = 0;
            if (!vms::utils::verifyTwoFactorTempTokenJwt(temp_token, user_id)) {
                return ApiUtils::createErrorResponse("Invalid or expired temp_token", 401, origin);
            }
            auto user_opt = repo.getUserById(user_id);

            if (user_opt && user_opt->two_factor_enabled) {
                if (vms::utils::TOTP::verifyCode(user_opt->two_factor_secret, code)) {
                    repo.updateLastLogin(user_opt->id);
                    std::string token = vms::utils::createAccessTokenJwt(*user_opt);
                    
                    database::AuditRepository audit;
                    audit.insertLog(user_opt->id, "LOGIN_2FA", "2FA verification successful");

                    return ApiUtils::createResponse(buildAuthResponse(false, user_opt, token), 200, origin);
                }
            }
        } catch (...) {}
        return ApiUtils::createErrorResponse("Invalid 2FA code", 401, origin);
    });
}

} // namespace api
} // namespace vms
