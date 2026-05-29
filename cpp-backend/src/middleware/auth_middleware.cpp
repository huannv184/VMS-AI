#include "middleware/auth_middleware.h"
#include "utils/logger.h"
#include "utils/api_utils.h"
#include "utils/sha256.h"
#include "utils/jwt_utils.h"
#include "utils/media_signer.h"
#include "database/db_state.h"
#include "database/user_repository.h"
#include "utils/config.h"
#include <chrono>
#include <algorithm>

namespace vms {
namespace middleware {

namespace {

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool isDbIndependentApiPath(const std::string& path, bool auth_enabled) {
    // /api/health is the soft-deprecated alias kept 200-always for legacy
    // probes. /api/health/live is the new liveness probe (must NEVER 503).
    // /api/health/ready is the new readiness probe — its handler emits 503
    // by design when DB is down, so it must NOT be short-circuited here.
    if (path == "/api/health" ||
        path == "/api/health/live" ||
        path == "/api/health/ready") {
        return true;
    }

    // Signed media routes do not need DB access when auth headers are absent.
    if (startsWith(path, "/api/snapshots/files/") ||
        startsWith(path, "/api/storage/faces/")) {
        return true;
    }

    if (auth_enabled) {
        return false;
    }

    if (path == "/api/system/stats" ||
        path == "/api/system/onvif/discover" ||
        path == "/api/system/network/scan/start" ||
        path == "/api/system/network/scan/status" ||
        path == "/api/system/network/scan/stop" ||
        path == "/api/system/network/cache" ||
        path == "/api/cameras/discover" ||
        startsWith(path, "/api/cameras/discover/status/") ||
        startsWith(path, "/api/cameras/discover/results/") ||
        startsWith(path, "/api/snapshots") ||
        startsWith(path, "/api/reid/")) {
        return true;
    }

    return startsWith(path, "/api/cameras/") && path.find("/hls/") != std::string::npos;
}

} // namespace

static bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

void AuthMiddleware::before_handle(crow::request& req, crow::response& res, context& ctx) {
    std::string path = req.url;
    const size_t query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }

    const bool auth_enabled = vms::Config::getInstance().getAuthConfig().enabled;

    if (req.method != crow::HTTPMethod::Options &&
        startsWith(path, "/api/") &&
        !isDbIndependentApiPath(path, auth_enabled) &&
        !vms::database::db_ready.load(std::memory_order_acquire)) {
        res = vms::api::ApiUtils::createErrorResponse("Database unavailable", 503,
                                                      vms::api::ApiUtils::resolveCorsOrigin(req));
        res.end();
        return;
    }

    // Check if authentication is enabled in config
    if (!auth_enabled) {
        return;
    }

    // Skip auth for OPTIONS (CORS) and public routes
    if (req.method == crow::HTTPMethod::Options) return;

    // SPA + static assets + /snapshots, /recordings, etc.: browsers load these without
    // Authorization. Only JSON API under /api/ (except explicit public routes below)
    // requires JWT for GET.
    if (req.method == crow::HTTPMethod::Get) {
        std::string path = req.url;
        const size_t q = path.find('?');
        if (q != std::string::npos) path = path.substr(0, q);
        if (path.find("/api/") != 0) {
            return;
        }
    }
    
    // 1. Skip Public API Routes (keep this list minimal)
    // BUG-C2 FIX: removed /api/events/fire-test — that endpoint persists a real
    // event and broadcasts WS, RuleEngine, AlertRouter chains. Leaving it on the
    // public allowlist let any unauthenticated client trigger fake alerts at will.
    if (path == "/api/auth/login" ||
        path == "/api/auth/2fa/verify" ||
        path == "/api/health" ||
        path == "/api/health/live" ||  // liveness probe: orchestrator must hit without creds
        path == "/api/health/ready" || // readiness probe: LB must hit without creds
        path == "/api/v1/metrics" ||   // H9: gate is route-level bearer token, not JWT
        path == "/api/system/streaming-config") {  // PUBLIC: frontend cần trước khi login
        return;
    }

    std::string token;

    // Media endpoints used by <img>/<video> may not be able to set Authorization headers.
    // For those endpoints ONLY, allow short-lived presigned URLs: ?exp=<unix_s>&sig=<hex>
    auto auth_header = req.get_header_value("Authorization");

    // H5: Fall back to HttpOnly cookie when no Authorization header is present.
    // This enables browser-native cookie auth without exposing the token to JS.
    if (auth_header.empty()) {
        const auto cookie_hdr = req.get_header_value("Cookie");
        const std::string prefix = "vms_session=";
        const auto pos = cookie_hdr.find(prefix);
        if (pos != std::string::npos) {
            const auto val_start = pos + prefix.size();
            const auto val_end   = cookie_hdr.find(';', val_start);
            const std::string cookie_token = (val_end == std::string::npos)
                ? cookie_hdr.substr(val_start)
                : cookie_hdr.substr(val_start, val_end - val_start);
            if (!cookie_token.empty()) {
                // Synthesize a Bearer header so the rest of the validation path is unchanged
                auth_header = "Bearer " + cookie_token;
            }
        }
    }

    if (auth_header.empty()) {
        const bool is_get = (req.method == crow::HTTPMethod::Get);
        if (is_get) {
            std::string p = path;
            const size_t q = p.find('?');
            if (q != std::string::npos) p = p.substr(0, q);

            const bool is_media =
                (p.find("/api/snapshots/files/") == 0) ||
                (p.find("/api/cameras/") == 0 && p.find("/frame") != std::string::npos) ||
                (p.find("/api/events/") == 0 && p.find("/video") != std::string::npos) ||
                (p.find("/api/recordings/") == 0 && p.find("/video") != std::string::npos) ||
                (p.find("/api/storage/") == 0);

            if (is_media) {
                auto exp_s = req.url_params.get("exp");
                auto sig = req.url_params.get("sig");
                if (exp_s && sig) {
                    try {
                        long long exp = std::stoll(std::string(exp_s));
                        vms::utils::MediaAccessScope media_scope;
                        if (auto scope = req.url_params.get("scope"); scope) media_scope.scope = std::string(scope);
                        if (auto camera_id = req.url_params.get("camera_id"); camera_id) {
                            media_scope.camera_id = std::stoi(std::string(camera_id));
                        }
                        if (auto resource_id = req.url_params.get("resource_id"); resource_id) {
                            media_scope.resource_id = std::string(resource_id);
                        }
                        if (auto role = req.url_params.get("role"); role) {
                            media_scope.role = std::string(role);
                        }
                        const bool strict_scope_required =
                            vms::Config::getInstance().getMediaSigningConfig().strict_scope_required;
                        if (strict_scope_required && media_scope.scope.empty()) {
                            res.code = 401;
                            res.body = R"({"error": "Presigned URL scope is required"})";
                            res.end();
                            return;
                        }

                        if (vms::utils::verifyPresign(p, exp, std::string(sig), media_scope)) {
                            return; // allow unsigned context for media download
                        }
                    } catch (...) {
                        // fall through to 401
                    }
                }
            }
        }

        res.code = 401;
        res.body = R"({"error": "Authorization header required"})";
        res.end();
        return;
    } else {
        // Validate "Bearer " prefix before substr to prevent crash on short/malformed headers
        if (auth_header.size() < 8 || auth_header.substr(0, 7) != "Bearer ") {
            res.code = 401;
            res.body = R"({"error": "Invalid authorization format. Expected: Bearer <token>"})";
            res.end();
            return;
        }
        token = auth_header.substr(7);
    }
    vms::core::User user;
    
    if (validateToken(token, user)) {
        ctx.user = user;
    } else {
        res.code = 401;
        res.body = R"({"error": "Invalid or expired token"})";
        res.end();
    }
}

void AuthMiddleware::after_handle(crow::request& req, crow::response& res, context& ctx) {
    // No-op
}

bool AuthMiddleware::validate(const crow::request& req) {
    auto auth_header = req.get_header_value("Authorization");
    // L4 FIX: Must check < 8 (not < 7). "Bearer " is 7 chars; a valid token
    // has at least one char after the space, so minimum valid header is 8 chars.
    if (auth_header.empty() || auth_header.size() < 8 || auth_header.substr(0, 7) != "Bearer ") {
        return false;
    }
    
    std::string token = auth_header.substr(7);
    vms::core::User user;
    return validateToken(token, user);
}

bool AuthMiddleware::validateToken(const std::string& token, vms::core::User& user_out) {
    // Supported tokens:
    // - JWT (preferred): "<header>.<payload>.<sig>"
    // - Legacy (temporary): "<prefix>-<user_id>-<timestamp_ms>.<sig>"

    // JWT path: two dots
    if (std::count(token.begin(), token.end(), '.') == 2) {
        vms::core::User jwt_user;
        if (!vms::utils::decodeAccessTokenJwtUser(token, jwt_user)) {
            return false;
        }
        // H2: Revalidate against DB — checks is_active, role_id, and token_version.
        // This catches: deactivated users, role demotions, and forced-logout (token_version bump).
        vms::database::UserRepository repo;
        auto db_user = repo.getUserById(jwt_user.id);
        if (!db_user || !db_user->is_active) {
            LOG_THROTTLED_WARN(5000, "JWT rejected: user {} is inactive or deleted", jwt_user.id);
            return false;
        }
        if (db_user->token_version != jwt_user.token_version) {
            LOG_THROTTLED_WARN(5000, "JWT rejected: token version mismatch for user {} "
                               "(token={}, db={})", jwt_user.id, jwt_user.token_version, db_user->token_version);
            return false;
        }
        // Use DB-authoritative role_id/role_name — never trust claims for permissions
        jwt_user.role_id   = db_user->role_id;
        jwt_user.role_name = db_user->role_name;
        user_out = jwt_user;
        return true;
    }

    // Legacy format: "<prefix>-<user_id>-<timestamp_ms>.<hmac>"
    // e.g. "local-access-token-1-1709380800000.abcdef123..."
    
    // FIX: Verify HMAC signature BEFORE doing any DB lookup
    auto dot_pos = token.rfind('.');
    if (dot_pos == std::string::npos) {
        LOG_THROTTLED_WARN(5000, "Invalid token format: missing HMAC signature");
        return false;
    }
    std::string raw_token = token.substr(0, dot_pos);
    std::string provided_sig = token.substr(dot_pos + 1);
    std::string expected_sig = vms::utils::SHA256::hash(raw_token + vms::Config::getInstance().getAuthConfig().secret_key);
    if (!constantTimeEquals(provided_sig, expected_sig)) {
        LOG_THROTTLED_WARN(5000, "Token HMAC verification failed");
        return false;
    }
    
    std::string suffix;
    
    if (raw_token.find("local-access-token-") == 0) {
        suffix = raw_token.substr(19); // After "local-access-token-"
    } else if (raw_token.find("ldap-access-token-") == 0) {
        suffix = raw_token.substr(18); // After "ldap-access-token-"
    } else {
        return false;
    }
    
    try {
        // Parse "<user_id>-<timestamp>"
        auto dash_pos = suffix.find('-');
        if (dash_pos == std::string::npos) {
            LOG_THROTTLED_WARN(5000, "Invalid token format: missing user_id separator");
            return false;
        }
        
        int user_id = std::stoi(suffix.substr(0, dash_pos));
        std::string timestamp_str = suffix.substr(dash_pos + 1);
        
        // H2: Revalidate against DB — checks is_active and token_version.
        vms::database::UserRepository repo2;
        auto db_user = repo2.getUserById(user_id);
        if (!db_user || !db_user->is_active) {
            LOG_THROTTLED_WARN(5000, "Legacy token rejected: user {} is inactive or deleted", user_id);
            return false;
        }

        // Legacy tokens don't carry a version, so we must assume they are version 0.
        // If the DB version has been bumped, all legacy tokens are invalidated.
        if (db_user->token_version != 0) {
            LOG_THROTTLED_WARN(5000, "Legacy token rejected: session revoked for user {} "
                               "(legacy tokens are invalidated when version > 0)", user_id);
            return false;
        }
        
        user_out = *db_user;
        
        // Validate token expiration (from config)
        long long token_ts = std::stoll(timestamp_str);
        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        const long long ttl_ms = static_cast<long long>(vms::Config::getInstance().getAuthConfig().token_expire_minutes) * 60LL * 1000LL;
        if (ttl_ms > 0 && now_ms - token_ts > ttl_ms) {
            LOG_THROTTLED_WARN(5000, "Token expired (age: {}ms)", now_ms - token_ts);
            return false;
        }
        
        // Look up user from DB
        vms::database::UserRepository repo;
        auto user_opt = repo.getUserById(user_id);
        if (!user_opt) {
            LOG_THROTTLED_WARN(5000, "Token references non-existent user_id: {}", user_id);
            return false;
        }
        
        if (!user_opt->is_active) {
            LOG_THROTTLED_WARN(5000, "Token references inactive user: {}", user_opt->username);
            return false;
        }
        
        user_out = *user_opt;
        return true;
        
    } catch (const std::exception& e) {
        LOG_THROTTLED_WARN(5000, "Invalid token format: {}", e.what());
        return false;
    }
}

} // namespace middleware
} // namespace vms
