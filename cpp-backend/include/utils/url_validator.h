// SSRF protection helper for outbound HTTP destinations (webhook, telegram,
// alarm-output, future: storage signed URLs, brand-API webhooks, etc.).
//
// Pre-fix the alert_router did naive substring matching against literal IP
// prefixes ("192.168.", "10.", ...) which silently allowed:
//   - IPv6 loopback / link-local         (`http://[::1]/`)
//   - 0.0.0.0 / shorthand IPv4
//   - 172.17–172.31  (RFC1918 /12 second halves)
//   - hostnames resolving to an internal IP
//   - cloud metadata endpoints other than 169.254.169.254 (AWS canonical only)
// This module resolves the URL host through getaddrinfo and checks every
// returned A/AAAA record against the deny-list. Hostnames whose DNS answer
// includes ANY internal address are refused — even if the answer also
// contains a public IP — to defeat split-horizon DNS rebind games.
//
// CAVEAT (TOCTOU): cURL re-resolves the hostname when it actually issues the
// request, so a fast DNS flip between this validation and the connect() can
// still let through an internal target. To fully close that gap, callers
// should (a) pin the resolved IP via CURLOPT_RESOLVE, OR (b) only accept URLs
// whose host is an IP literal. Pinning is a follow-up; for now isInternalUrl
// closes the operator-misconfiguration class of SSRF, not the active-attacker
// class. Document the residual risk if the webhook URL comes from
// privileged-but-untrusted input.

#pragma once

#include <string>

namespace vms {
namespace utils {

struct UrlValidation {
    bool internal = false;        // true if any resolved IP is private/loopback/link-local/metadata
    std::string reason;           // human-readable, safe to log
    std::string host;             // host segment as parsed
    std::string resolved_ip;      // first resolved address (informational + TOCTOU pin)
    int port = 0;                 // explicit port if present in URL, else scheme default (80/443/0)
    bool resolved_ip_is_v6 = false; // true if resolved_ip is IPv6 (controls bracket formatting)
};

// Returns {internal=true, reason=...} when URL targets a private/loopback/
// link-local/cloud-metadata destination. Hostnames are resolved via
// getaddrinfo; resolution failure → internal=true (fail-closed).
//
// Caller convention: refuse to send when result.internal is true and the
// URL came from operator config OR untrusted input. Routes that legitimately
// target the LAN (e.g. alarm-output relay) should NOT use this — see
// triggerAlarmOutput in alert_router.cpp for the exception pattern.
UrlValidation isInternalUrl(const std::string& url);

// Build a CURLOPT_RESOLVE entry "host:port:ip" pinning the hostname to the
// resolved IP we just verified is public. cURL will skip its own DNS lookup
// and connect straight to this IP — defeating the DNS-rebind TOCTOU window
// between getaddrinfo() and connect(). Returns empty string when v.host/
// v.resolved_ip are missing, the port is zero, or v.internal is true (caller
// should already have bailed out in that case).
//
// Caller convention:
//   auto entry = vms::utils::buildResolveEntry(v);
//   struct curl_slist* resolve_list = curl_slist_append(nullptr, entry.c_str());
//   curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
//   ...
//   curl_slist_free_all(resolve_list);
//
// Hostname is preserved for SNI / TLS cert validation; only the A/AAAA lookup
// is bypassed.
std::string buildResolveEntry(const UrlValidation& v);

} // namespace utils
} // namespace vms
