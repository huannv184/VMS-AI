#include "api/user_controller.h"
#include "utils/api_utils.h"
#include "utils/config.h"
#include "utils/logger.h"
#include "utils/session_cookie.h"
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
#include "database/user_repository.h"
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

// Legacy SHA256 verifier — used ONLY during migration on login.
// DO NOT use this for creating new hashes.
static std::string hashPasswordV2(const std::string& password, const std::string& username, const std::string& salt) {
    return vms::utils::SHA256::hash(password + username + salt);
}

// H6 + BUG-C1 FIX: hashing returns BOTH the stored hash and the salt column
// value, so create/change/reset all share one consistent format that login
// and the change-password verifier both recognise. Previously the fallback
// produced "<sha256>:<salt>" while the login verifier looked for
// hashPasswordV2(pwd, username, salt) — those two formulas never matched, so
// any user created without argon2 could not log in.
struct HashedPassword {
    std::string hash;
    std::string salt; // For argon2 we use literal "argon2" — salt is embedded in the hash string
};

static HashedPassword hashPasswordWithSalt(const std::string& password, const std::string& username) {
#ifdef VMS_HAS_ARGON2
    const std::string salt = vms::utils::generateArgon2Salt();
    return { vms::utils::hashPasswordArgon2(password, salt), "argon2" };
#else
    const std::string salt = generateSaltHex();
    // Use the same formula login verifies against (hashPasswordV2) so newly
    // created users round-trip cleanly through the legacy code path until
    // argon2 is wired in.
    return { hashPasswordV2(password, username, salt), salt };
#endif
}

// BUG-H1 FIX: unified verifier handles every accepted password format.
// Previously change-password only checked legacy v2 / global-salt — argon2
// users (or even default-admin users post-migration) could never change their
// own password despite being able to log in.
static bool verifyUserPassword(const std::string& password, const vms::core::User& user) {
    if (user.password_hash.empty()) return false;
    if (vms::utils::isArgon2Hash(user.password_hash)) {
        return vms::utils::verifyPasswordArgon2(password, user.password_hash);
    }
    // Per-user salted SHA256 (current legacy + new fallback).
    // Treat the literal markers "argon2"/"sha256" as "no actual salt stored" —
    // those are migration markers, not real salts.
    if (!user.salt.empty() && user.salt != "argon2" && user.salt != "sha256") {
        if (hashPasswordV2(password, user.username, user.salt) == user.password_hash) return true;
    }
    if (vms::utils::SHA256::hash(password + user.username + "VMS_GLOBAL_SALT") == user.password_hash) return true;
    if (vms::utils::SHA256::hash(password) == user.password_hash) return true;
    return false;
}

static bool shouldUseSecureCookie(const crow::request& req) {
    const std::string forwarded_proto = req.get_header_value("X-Forwarded-Proto");
    if (!forwarded_proto.empty()) {
        return forwarded_proto == "https";
    }

    const std::string origin = req.get_header_value("Origin");
    return origin.rfind("https://", 0) == 0;
}

// Build a consistent HttpOnly session cookie for all login flows.
// Pure formatting lives in `vms::utils::formatSessionCookie` (header-only,
// testable without crow). This wrapper only resolves the runtime TTL and
// the Secure flag from the request.
static std::string buildSessionCookie(const std::string& token,
                                      const crow::request& req) {
    const int ttl_sec = vms::Config::getInstance().getAuthConfig().token_expire_minutes * 60;
    return vms::utils::formatSessionCookie(token, ttl_sec, shouldUseSecureCookie(req));
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
    
    // LINT-ALLOW-NO-AUTH: auth-flow — login is the entry point, must accept unauthed callers.
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
                bool argon2_match = vms::utils::isArgon2Hash(user_opt->password_hash) &&
                                    vms::utils::verifyPasswordArgon2(password, user_opt->password_hash);
                bool legacy_match = !argon2_match && verifyUserPassword(password, *user_opt);

                if (argon2_match || legacy_match) {
                    local_auth_success = true;

                    // Auto-rehash legacy formats up to whichever new format we support.
                    if (legacy_match) {
                        try {
                            auto fresh = hashPasswordWithSalt(password, user_opt->username);
                            repo.updatePasswordWithSalt(user_opt->id, fresh.hash, fresh.salt);
                            user_opt->password_hash = fresh.hash;
                            user_opt->salt = fresh.salt;
                            LOG_INFO("[Auth] Re-hashed password for '{}'", user_opt->username);
                        } catch (const std::exception& e) {
                            LOG_WARN("[Auth] Re-hash failed for '{}': {}", user_opt->username, e.what());
                        }
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

                // SEC-001: when admin is still on the factory-default password
                // we MUST NOT issue an access token or session cookie. Doing so
                // would let any HTTP client that knew "admin/admin" use the API
                // freely until they happen to navigate to a UI that surfaces the
                // password_change_required flag — which the previous frontend
                // never did. Instead emit a short-lived password_change_pending
                // JWT that only the change-password-on-login endpoint accepts.
                if (must_change_password) {
                    database::AuditRepository audit;
                    audit.insertLog(user_opt->id, "LOGIN_DEFAULT_PWD",
                                    "Login blocked: default password change required");
                    return ApiUtils::createResponse(
                        buildAuthResponse(
                            false,
                            user_opt,
                            "",
                            vms::utils::createPasswordChangeTempTokenJwt(*user_opt),
                            true
                        ),
                        200,
                        origin
                    );
                }

                repo.updateLastLogin(user_opt->id);
                std::string token = vms::utils::createAccessTokenJwt(*user_opt);

                database::AuditRepository audit;
                audit.insertLog(user_opt->id, "LOGIN", "Local login successful");

                auto resp = ApiUtils::createResponse(
                    buildAuthResponse(false, user_opt, token, "", must_change_password),
                    200, origin);
                resp.set_header("Set-Cookie", buildSessionCookie(token, req));
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
                        // BUG-21 FIX: clear rate-limit window and audit-log this login.
                        // Local login does both; LDAP path used to skip both, leaving
                        // failed-attempt counters live and creating an audit gap that
                        // hid LDAP-user activity.
                        vms::utils::RateLimiter::getInstance().recordLoginSuccess(client_ip, username);
                        try {
                            database::AuditRepository audit;
                            audit.insertLog(user_opt->id, "LOGIN", "LDAP login successful");
                        } catch (...) {}
                        std::string token = vms::utils::createAccessTokenJwt(*user_opt);
                        auto resp = ApiUtils::createResponse(buildAuthResponse(false, user_opt, token), 200, origin);
                        resp.set_header("Set-Cookie", buildSessionCookie(token, req));
                        return resp;
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

    // LINT-ALLOW-NO-AUTH: auth-flow — logout must work on a stale token (best-effort cookie clear).
    // POST /api/auth/logout
    CROW_ROUTE(app, "/api/auth/logout")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        // Bump token_version so the current JWT is rejected by auth middleware on next use.
        // This invalidates all bearer tokens for this user immediately,
        // regardless of whether the client discards them.
        vms::core::User caller;
        const std::string auth_header = req.get_header_value("Authorization");
        std::string bearer;
        if (auth_header.rfind("Bearer ", 0) == 0) bearer = auth_header.substr(7);
        if (bearer.empty()) {
            // Also try cookie
            const std::string cookie_hdr = req.get_header_value("Cookie");
            const std::string key = "vms_session=";
            auto pos = cookie_hdr.find(key);
            if (pos != std::string::npos) {
                auto end = cookie_hdr.find(';', pos + key.size());
                bearer = cookie_hdr.substr(pos + key.size(),
                    end == std::string::npos ? std::string::npos : end - pos - key.size());
            }
        }
        if (!bearer.empty() && vms::utils::decodeAccessTokenJwtUser(bearer, caller) && caller.id > 0) {
            database::UserRepository repo;
            repo.bumpTokenVersion(caller.id);
        }

        // Expire the session cookie so the browser drops it immediately.
        auto resp = ApiUtils::createResponse(json::object(), 200, origin);
        resp.set_header("Set-Cookie",
            "vms_session=; HttpOnly; SameSite=Strict; Path=/api; Max-Age=0");
        return resp;
    });

    // /api/users
    CROW_ROUTE(app, "/api/users")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requireAdmin(ctx, origin)) return std::move(*err);

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
                
                {
                    auto fresh = hashPasswordWithSalt(raw_pass, user.username);
                    user.password_hash = fresh.hash;
                    user.salt = fresh.salt;
                }
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
        if (auto err = ApiUtils::requireAdmin(ctx, origin)) return std::move(*err);

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

            // BUG-H1 FIX: use the unified verifier so users on argon2 / default-admin
            // bare-SHA256 / per-user salted SHA256 / global-salt all succeed when the
            // old password is correct. Previously only legacy v2 / global salt were
            // checked → argon2-hashed users got "Invalid old password" forever.
            if (!verifyUserPassword(old_pass, *user)) {
                return ApiUtils::createErrorResponse("Invalid old password", 401, origin);
            }

            auto fresh = hashPasswordWithSalt(new_pass, user->username);
            if (repo.updatePasswordWithSalt(user->id, fresh.hash, fresh.salt)) {
                // BUG-H3 FIX: invalidate any existing JWTs for this user. Before this
                // bump, an attacker who had captured the user's token kept full access
                // even after the user reacted by changing the password.
                repo.bumpTokenVersion(user->id);
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
        if (auto err = ApiUtils::requireAdmin(ctx, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            std::string new_pass = body.value("new_password", "");
            if (new_pass.size() < 8 || new_pass.size() > 256)
                return ApiUtils::createErrorResponse("new_password must be 8-256 characters", 400, origin);
            database::UserRepository repo;
            auto u = repo.getUserById(id);
            if (!u.has_value()) return ApiUtils::createErrorResponse("User not found", 404, origin);
            
            auto fresh = hashPasswordWithSalt(new_pass, u->username);
            if (repo.updatePasswordWithSalt(id, fresh.hash, fresh.salt)) {
                // BUG-H3 FIX: bump token_version on admin reset too, so the target
                // user's existing sessions are dropped.
                repo.bumpTokenVersion(id);
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
        if (auto err = ApiUtils::requireAdmin(ctx, origin)) return std::move(*err);

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

    // LINT-ALLOW-NO-AUTH: auth-flow — 2FA verify runs against a 2fa_pending token, not a session.
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
                    // SEC-001: same default-password block applies after 2FA.
                    // 2FA proves possession but not that the password has been
                    // rotated; admin/admin + correct TOTP must still go through
                    // the change-password gate.
                    const bool must_change_password =
                        user_opt->username == "admin" &&
                        vms::database::default_password_active.load(std::memory_order_acquire);

                    if (must_change_password) {
                        database::AuditRepository audit;
                        audit.insertLog(user_opt->id, "LOGIN_2FA_DEFAULT_PWD",
                                        "2FA OK but default password change required");
                        return ApiUtils::createResponse(
                            buildAuthResponse(
                                false,
                                user_opt,
                                "",
                                vms::utils::createPasswordChangeTempTokenJwt(*user_opt),
                                true
                            ),
                            200,
                            origin
                        );
                    }

                    repo.updateLastLogin(user_opt->id);
                    std::string token = vms::utils::createAccessTokenJwt(*user_opt);

                    database::AuditRepository audit;
                    audit.insertLog(user_opt->id, "LOGIN_2FA", "2FA verification successful");

                    auto resp = ApiUtils::createResponse(buildAuthResponse(false, user_opt, token), 200, origin);
                    resp.set_header("Set-Cookie", buildSessionCookie(token, req));
                    return resp;
                }
            }
        } catch (...) {}
        return ApiUtils::createErrorResponse("Invalid 2FA code", 401, origin);
    });

    // SEC-001: POST /api/auth/change-password-on-login
    // Completes the default-password change gate. Accepts a
    // password_change_pending JWT (issued by /login or /2fa/verify when
    // default_password_active was set) plus the new password. On success
    // updates the password, clears the global flag, bumps token_version
    // (so any leaked access token from before the rotation is dead), and
    // issues the real access token + session cookie.
    // LINT-ALLOW-NO-AUTH: auth-flow — runs on the password_change_pending JWT only (SEC-002 default-password gate).
    CROW_ROUTE(app, "/api/auth/change-password-on-login")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        try {
            auto body = json::parse(req.body);
            std::string temp_token = body.value("temp_token", "");
            std::string new_pass   = body.value("new_password", "");

            if (temp_token.empty()) {
                return ApiUtils::createErrorResponse("temp_token is required", 400, origin);
            }
            if (new_pass.size() < 8 || new_pass.size() > 256) {
                return ApiUtils::createErrorResponse("new_password must be 8-256 characters", 400, origin);
            }

            int user_id = 0;
            if (!vms::utils::verifyPasswordChangeTempTokenJwt(temp_token, user_id)) {
                return ApiUtils::createErrorResponse("Invalid or expired temp_token", 401, origin);
            }

            database::UserRepository repo;
            auto user = repo.getUserById(user_id);
            if (!user.has_value()) {
                return ApiUtils::createErrorResponse("User not found", 404, origin);
            }

            // Reject reusing the factory-default password.
            if (verifyUserPassword(new_pass, *user)) {
                return ApiUtils::createErrorResponse(
                    "New password must differ from the current one", 400, origin);
            }

            auto fresh = hashPasswordWithSalt(new_pass, user->username);
            if (!repo.updatePasswordWithSalt(user->id, fresh.hash, fresh.salt)) {
                return ApiUtils::createErrorResponse("Failed to update password", 500, origin);
            }

            // Invalidate any stale tokens issued before this rotation.
            repo.bumpTokenVersion(user->id);
            // Reflect the bump in the in-memory user we are about to sign a
            // fresh access token for — otherwise the new token's `ver` claim
            // would still match the pre-bump version.
            ++user->token_version;
            user->password_hash = fresh.hash;
            user->salt = fresh.salt;

            if (user->username == "admin" &&
                vms::database::default_password_active.load(std::memory_order_acquire)) {
                vms::database::default_password_active.store(false, std::memory_order_release);
                LOG_INFO("SEC-001: Admin default password cleared via change-on-login");
            }

            database::AuditRepository audit;
            audit.insertLog(user->id, "CHANGE_PASSWORD_ON_LOGIN",
                            "Default password rotated during login");

            repo.updateLastLogin(user->id);
            std::string token = vms::utils::createAccessTokenJwt(*user);

            auto resp = ApiUtils::createResponse(
                buildAuthResponse(false, user, token, "", false),
                200, origin);
            resp.set_header("Set-Cookie", buildSessionCookie(token, req));
            return resp;
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 400, origin);
        }
    });
}

} // namespace api
} // namespace vms
