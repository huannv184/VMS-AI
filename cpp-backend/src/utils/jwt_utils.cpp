#include "utils/jwt_utils.h"

#include "utils/config.h"
#include "utils/logger.h"

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace vms::utils {

namespace {

static std::string generateJti() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);
    const char* hex = "0123456789abcdef";
    char buf[37];
    for (int i = 0; i < 36; i++) buf[i] = hex[dist(rng)];
    buf[8] = '-'; buf[13] = '-'; buf[14] = '4';
    buf[18] = '-'; buf[19] = hex[8 + (dist(rng) % 4)];
    buf[23] = '-'; buf[36] = '\0';
    return std::string(buf);
}

template <typename Builder>
std::string signUserToken(const vms::core::User& user,
                          const std::chrono::system_clock::duration& ttl,
                          const std::string& token_use,
                          Builder&& builder) {
    using jwt_traits = jwt::traits::nlohmann_json;
    const auto& auth = vms::Config::getInstance().getAuthConfig();
    const auto now = std::chrono::system_clock::now();
    const auto exp = now + ttl;

    auto token = jwt::create<jwt_traits>()
        .set_type("JWT")
        .set_issuer("vms-backend")
        .set_subject(std::to_string(user.id))
        .set_id(generateJti())
        .set_issued_at(now)
        .set_expires_at(exp)
        .set_payload_claim("username", jwt::basic_claim<jwt_traits>(user.username))
        .set_payload_claim("role_id", jwt::basic_claim<jwt_traits>(std::to_string(user.role_id)))
        .set_payload_claim("token_use", jwt::basic_claim<jwt_traits>(token_use))
        // H2: embed token_version so revocation is detectable without full DB lookup
        .set_payload_claim("ver", jwt::basic_claim<jwt_traits>(std::to_string(user.token_version)));

    builder(token);
    return token.sign(jwt::algorithm::hs256{auth.secret_key});
}

template <typename Predicate>
bool verifyJwtForUse(const std::string& token, int& user_id_out, Predicate&& predicate) noexcept {
    try {
        using jwt_traits = jwt::traits::nlohmann_json;
        const auto& auth = vms::Config::getInstance().getAuthConfig();

        auto decoded = jwt::decode<jwt_traits>(token);
        auto verifier = jwt::verify<jwt_traits>()
            .allow_algorithm(jwt::algorithm::hs256{auth.secret_key})
            .with_issuer("vms-backend");

        verifier.verify(decoded);

        std::string token_use = "access";
        if (decoded.has_payload_claim("token_use")) {
            token_use = decoded.get_payload_claim("token_use").as_string();
        }
        if (!predicate(token_use)) {
            return false;
        }

        const auto sub = decoded.get_subject();
        if (sub.empty()) return false;
        user_id_out = std::stoi(sub);
        return true;
    } catch (const std::exception& e) {
        LOG_DEBUG("JWT verify failed: {}", e.what());
        return false;
    } catch (...) {
        LOG_DEBUG("JWT verify failed: unknown error");
        return false;
    }
}

template <typename Predicate>
bool decodeJwtUserForUse(const std::string& token, vms::core::User& user_out, Predicate&& predicate) noexcept {
    try {
        using jwt_traits = jwt::traits::nlohmann_json;
        const auto& auth = vms::Config::getInstance().getAuthConfig();

        auto decoded = jwt::decode<jwt_traits>(token);
        auto verifier = jwt::verify<jwt_traits>()
            .allow_algorithm(jwt::algorithm::hs256{auth.secret_key})
            .with_issuer("vms-backend");

        verifier.verify(decoded);

        std::string token_use = "access";
        if (decoded.has_payload_claim("token_use")) {
            token_use = decoded.get_payload_claim("token_use").as_string();
        }
        if (!predicate(token_use)) {
            return false;
        }

        const auto sub = decoded.get_subject();
        if (sub.empty()) {
            return false;
        }

        user_out = vms::core::User{};
        user_out.id = std::stoi(sub);
        user_out.username = decoded.get_payload_claim("username").as_string();
        user_out.role_id = std::stoi(decoded.get_payload_claim("role_id").as_string());
        user_out.role_name = decoded.has_payload_claim("role_name")
            ? decoded.get_payload_claim("role_name").as_string()
            : std::string{};
        user_out.is_active = !decoded.has_payload_claim("is_active") ||
                             decoded.get_payload_claim("is_active").as_boolean();
        user_out.two_factor_enabled = decoded.has_payload_claim("two_factor_enabled") &&
                                      decoded.get_payload_claim("two_factor_enabled").as_boolean();
        // H2: extract token version for DB revalidation in AuthMiddleware::validateToken
        user_out.token_version = decoded.has_payload_claim("ver")
            ? std::stoi(decoded.get_payload_claim("ver").as_string())
            : 0;
        return user_out.is_active;
    } catch (const std::exception& e) {
        LOG_DEBUG("JWT user decode failed: {}", e.what());
        return false;
    } catch (...) {
        LOG_DEBUG("JWT user decode failed: unknown error");
        return false;
    }
}

} // namespace

std::string createAccessTokenJwt(const vms::core::User& user) {
    const auto& auth = vms::Config::getInstance().getAuthConfig();
    return signUserToken(
        user,
        std::chrono::minutes(auth.token_expire_minutes),
        "access",
        [&user](auto& token) {
            using jwt_traits = jwt::traits::nlohmann_json;
            token
                .set_payload_claim("role_name", jwt::basic_claim<jwt_traits>(user.role_name))
                .set_payload_claim("is_active", jwt::basic_claim<jwt_traits>(user.is_active))
                .set_payload_claim("two_factor_enabled", jwt::basic_claim<jwt_traits>(user.two_factor_enabled));
        }
    );
}

std::string createTwoFactorTempTokenJwt(const vms::core::User& user) {
    return signUserToken(
        user,
        std::chrono::minutes(5),
        "2fa_pending",
        [](auto& token) {
            using jwt_traits = jwt::traits::nlohmann_json;
            token.set_payload_claim("requires_2fa", jwt::basic_claim<jwt_traits>(true));
        }
    );
}

bool verifyAccessTokenJwt(const std::string& token, int& user_id_out) {
    return verifyJwtForUse(token, user_id_out, [](const std::string& token_use) {
        return token_use.empty() || token_use == "access";
    });
}

bool decodeAccessTokenJwtUser(const std::string& token, vms::core::User& user_out) {
    return decodeJwtUserForUse(token, user_out, [](const std::string& token_use) {
        return token_use.empty() || token_use == "access";
    });
}

bool verifyTwoFactorTempTokenJwt(const std::string& token, int& user_id_out) {
    return verifyJwtForUse(token, user_id_out, [](const std::string& token_use) {
        return token_use == "2fa_pending";
    });
}

std::string createPasswordChangeTempTokenJwt(const vms::core::User& user) {
    return signUserToken(
        user,
        std::chrono::minutes(5),
        "password_change_pending",
        [](auto& token) {
            using jwt_traits = jwt::traits::nlohmann_json;
            token.set_payload_claim("requires_password_change",
                                    jwt::basic_claim<jwt_traits>(true));
        }
    );
}

bool verifyPasswordChangeTempTokenJwt(const std::string& token, int& user_id_out) {
    return verifyJwtForUse(token, user_id_out, [](const std::string& token_use) {
        return token_use == "password_change_pending";
    });
}

std::string createWsTicketJwt(const vms::core::User& user, const std::string& client_ip) {
    return signUserToken(
        user,
        std::chrono::seconds(30),
        "ws_ticket",
        [&client_ip](auto& token) {
            using jwt_traits = jwt::traits::nlohmann_json;
            if (!client_ip.empty()) {
                token.set_payload_claim("ip", jwt::basic_claim<jwt_traits>(client_ip));
            }
        }
    );
}

bool decodeWsTicketJwt(const std::string& token, vms::core::User& user_out, std::string& jti_out, const std::string& client_ip) {
    try {
        using jwt_traits = jwt::traits::nlohmann_json;
        auto decoded = jwt::decode<jwt_traits>(token);
        
        if (decoded.has_id()) {
            jti_out = decoded.get_id();
        } else {
            return false; // JTI is required for one-time tracking
        }

        if (!client_ip.empty() && decoded.has_payload_claim("ip")) {
            std::string bound_ip = decoded.get_payload_claim("ip").as_string();
            // Handle IPV6 mapping like ::ffff:192.168.1.1 
            std::string normalized_bound = bound_ip;
            if (normalized_bound.substr(0, 7) == "::ffff:") normalized_bound = normalized_bound.substr(7);
            
            std::string normalized_client = client_ip;
            if (normalized_client.substr(0, 7) == "::ffff:") normalized_client = normalized_client.substr(7);

            if (normalized_client != "127.0.0.1" && normalized_client != "::1" && normalized_client != "localhost" && normalized_bound != normalized_client) {
                LOG_WARN("WS Ticket IP mismatch. Bound: {}, Client: {}", normalized_bound, normalized_client);
                return false;
            }
        }
    } catch (...) {
        return false;
    }

    return decodeJwtUserForUse(token, user_out, [](const std::string& token_use) {
        return token_use == "ws_ticket";
    });
}

} // namespace vms::utils
