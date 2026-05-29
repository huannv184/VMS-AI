#pragma once

// Pure helper for the /api/v1/metrics bearer-token gate (H9).
//
// Why a separate token instead of reusing JWT: Prometheus scrapes every
// 15-60 seconds and expects a static bearer token via
// `bearer_token_file` config. A JWT that expires every 8 hours would
// either need an external refresher (operationally painful) or
// effectively-static long-lived tokens (defeats JWT purpose). A single
// static token compared in constant time is the standard pattern.
//
// Gate behaviour:
//   * empty configured_token  → endpoint stays effectively public (the
//     route still has to be in AuthMiddleware's public allowlist for
//     this to matter). Match current "restrict via network ACL" mode.
//   * non-empty configured_token → require `Authorization: Bearer
//     <token>` with an exact match. Constant-time compare protects
//     against length+timing oracles.

#include <cstddef>
#include <string>
#include <string_view>

namespace vms {
namespace utils {

// Returns true iff the metrics request should be served.
//   configured_token : the server-side secret (Config::SecurityConfig::metrics_token).
//                      Empty disables the gate entirely.
//   auth_header      : raw value of the Authorization request header.
inline bool isMetricsRequestAuthorized(std::string_view configured_token,
                                       std::string_view auth_header) {
    // No gate configured → any request passes (caller must still have
    // a network-level guard; that's the operator's choice).
    if (configured_token.empty()) {
        return true;
    }

    // Must look like "Bearer <token>" exactly. No leading whitespace,
    // no case variation on the scheme — Prometheus emits it exactly.
    constexpr std::string_view kPrefix = "Bearer ";
    if (auth_header.size() != kPrefix.size() + configured_token.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kPrefix.size(); ++i) {
        if (auth_header[i] != kPrefix[i]) return false;
    }
    const std::string_view provided = auth_header.substr(kPrefix.size());

    // Length matches by construction (we already gated on total size).
    // Constant-time XOR-fold: never short-circuit on first byte mismatch.
    unsigned char diff = 0;
    for (std::size_t i = 0; i < provided.size(); ++i) {
        diff |= static_cast<unsigned char>(provided[i]) ^
                static_cast<unsigned char>(configured_token[i]);
    }
    return diff == 0;
}

} // namespace utils
} // namespace vms
