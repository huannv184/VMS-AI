#pragma once

#include <string>
#include "core/user.h"

namespace vms::utils {

/**
 * Create a signed JWT access token for the given user.
 */
std::string createAccessTokenJwt(const vms::core::User& user);

/**
 * Create a short-lived JWT used only for completing 2FA after password validation.
 */
std::string createTwoFactorTempTokenJwt(const vms::core::User& user);

/**
 * Verify a JWT access token and extract user_id (from "sub") on success.
 * Returns true if the token is valid and not expired.
 */
bool verifyAccessTokenJwt(const std::string& token, int& user_id_out);

/**
 * Verify a JWT access token and decode the O(1) user claims needed by middleware.
 * Falls back to false if the token is valid but does not carry the required claims.
 */
bool decodeAccessTokenJwtUser(const std::string& token, vms::core::User& user_out);

/**
 * Verify a short-lived JWT issued for pending 2FA completion.
 */
bool verifyTwoFactorTempTokenJwt(const std::string& token, int& user_id_out);

/**
 * Create a short-lived, single-use ticket for WebSocket authentication bound to an IP.
 */
std::string createWsTicketJwt(const vms::core::User& user, const std::string& client_ip = "");

/**
 * Verify and decode a WS ticket, enforcing IP binding if client_ip is provided,
 * and extracting the JWT ID (jti) to prevent replay attacks.
 */
bool decodeWsTicketJwt(const std::string& token, vms::core::User& user_out, std::string& jti_out, const std::string& client_ip = "");

} // namespace vms::utils

