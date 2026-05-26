#pragma once

// Pure helpers for production-mode security headers.
//
// The backend itself speaks plain HTTP — TLS is terminated at a reverse
// proxy (nginx / caddy / IIS). The proxy forwards `X-Forwarded-Proto:
// https` so the backend can detect when the client connection was
// actually encrypted. Trust is gated on the same `security.trusted_proxies`
// allowlist used for X-Forwarded-For client-IP resolution: if no proxy
// is configured, X-Forwarded-Proto is ignored and HSTS is NOT emitted
// (a dev / no-proxy box returning HSTS would poison browsers into
// refusing future plain-HTTP connections — a multi-month foot-gun).
//
// This header is intentionally pure (no Crow, no Config singleton, no
// strings parsed from config) so the policy matrix is unit-testable.
// The Crow middleware in vms_app.h pulls the inputs from the request +
// Config and calls shouldEmitHsts() to decide.

#include <string>
#include <string_view>

namespace vms {
namespace utils {

// Returns true iff the response should carry an HSTS header.
//
// Both gates must be true:
//   * `trusted_proxies_configured` — operator has declared at least one
//     reverse-proxy IP in `security.trusted_proxies`. Without that, the
//     `xfp_header` could be spoofed by any client and HSTS would lock
//     plain-HTTP dev/admin tools out of the browser.
//   * `xfp_header` equals "https" (case-insensitive) — the proxy
//     told us the *client* leg was TLS-terminated. http/empty/anything-
//     else means we don't know, so we don't claim TLS.
inline bool shouldEmitHsts(bool trusted_proxies_configured,
                           std::string_view xfp_header) {
    if (!trusted_proxies_configured) return false;
    if (xfp_header.size() != 5) return false;  // "https" is 5 chars; quick filter
    // Case-insensitive compare to literal "https".
    static constexpr char kHttps[5] = {'h', 't', 't', 'p', 's'};
    for (size_t i = 0; i < 5; ++i) {
        char a = xfp_header[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != kHttps[i]) return false;
    }
    return true;
}

// Canonical HSTS header value used across the backend. One year max-age,
// includeSubDomains. Not `preload` — operators should opt in to that
// only after they've vetted every subdomain manually.
inline const char* canonicalHstsValue() {
    return "max-age=31536000; includeSubDomains";
}

} // namespace utils
} // namespace vms
