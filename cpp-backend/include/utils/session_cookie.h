#pragma once

#include <string>

namespace vms::utils {

// Pure session-cookie formatter — no Config / crow dependency so the
// contract is testable directly. user_controller.cpp wraps this with
// Config TTL + crow X-Forwarded-Proto detection.
//
// Output shape:
//   "vms_session=<token>; HttpOnly; SameSite=Strict; Path=/api; Max-Age=<ttl>"
// plus "; Secure" if `secure` is true. Used both for login (ttl > 0) and
// logout (ttl == 0 to ask the browser to drop the cookie).
inline std::string formatSessionCookie(const std::string& token,
                                       int ttl_seconds,
                                       bool secure) {
    std::string val = "vms_session=" + token +
        "; HttpOnly; SameSite=Strict; Path=/api; Max-Age=" + std::to_string(ttl_seconds);
    if (secure) val += "; Secure";
    return val;
}

}  // namespace vms::utils
