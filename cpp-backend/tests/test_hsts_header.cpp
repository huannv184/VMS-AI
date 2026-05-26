// Pin the HSTS gate matrix in utils/security_headers.h.
//
// HSTS is the kind of header you cannot un-emit — once a browser caches
// `Strict-Transport-Security: max-age=31536000`, every plain-HTTP
// connection to that host fails for the next year. So the policy needs
// a sharp guard: emit ONLY when an operator-declared reverse proxy
// reports the client leg was TLS. Anything else (no proxy, no XFP
// header, XFP=http) must be silent.
//
// This file is the contract.

#include <gtest/gtest.h>

#include "utils/security_headers.h"

using vms::utils::shouldEmitHsts;
using vms::utils::canonicalHstsValue;

TEST(Hsts, EmitsWhenProxiesConfiguredAndXfpHttps) {
    EXPECT_TRUE(shouldEmitHsts(true, "https"));
}

TEST(Hsts, EmitsWhenXfpHttpsCaseInsensitive) {
    // RFC 7230 §3.2 says field values are case-insensitive — proxies
    // in the wild sometimes upper-case (Caddy used to). The gate
    // must tolerate that.
    EXPECT_TRUE(shouldEmitHsts(true, "HTTPS"));
    EXPECT_TRUE(shouldEmitHsts(true, "Https"));
    EXPECT_TRUE(shouldEmitHsts(true, "hTtPs"));
}

TEST(Hsts, SilentWhenProxiesEmpty) {
    // The most important guard: never emit HSTS without an operator
    // declaration that there IS a reverse proxy. Without that, any
    // client could spoof XFP=https and trick us into poisoning the
    // browser cache against our own dev tools for a year.
    EXPECT_FALSE(shouldEmitHsts(false, "https"));
    EXPECT_FALSE(shouldEmitHsts(false, "HTTPS"));
}

TEST(Hsts, SilentWhenXfpEmpty) {
    // Direct hit to the backend with no proxy — XFP is missing.
    // (Crow returns empty string for absent headers, not the literal
    // word "absent".)
    EXPECT_FALSE(shouldEmitHsts(true, ""));
}

TEST(Hsts, SilentWhenXfpHttp) {
    // Proxy explicitly told us the client leg was plain HTTP. Don't
    // claim TLS we don't have.
    EXPECT_FALSE(shouldEmitHsts(true, "http"));
    EXPECT_FALSE(shouldEmitHsts(true, "HTTP"));
}

TEST(Hsts, SilentWhenXfpUnknownValue) {
    // Anything that isn't exactly "https" (case-insensitive). Includes
    // multi-value XFP headers like "https, http" — backend should NOT
    // try to "parse" the list and pick a winner. The proxy is the
    // authoritative source; if it didn't give a clean "https" value,
    // we don't emit.
    EXPECT_FALSE(shouldEmitHsts(true, "https, http"));
    EXPECT_FALSE(shouldEmitHsts(true, "wss"));
    EXPECT_FALSE(shouldEmitHsts(true, "tls"));
    EXPECT_FALSE(shouldEmitHsts(true, " https"));   // leading space
    EXPECT_FALSE(shouldEmitHsts(true, "https "));   // trailing space
}

TEST(Hsts, CanonicalValueShape) {
    // Pin the header value too — operators reading network captures
    // need to recognise the exact string. Changing this string forces
    // an HSTS preload re-vetting cycle.
    const std::string v = canonicalHstsValue();
    EXPECT_EQ(v, "max-age=31536000; includeSubDomains");
}

TEST(Hsts, NoSideEffects) {
    // Same inputs → same answer, no clocks, no atomics, no globals.
    const bool a = shouldEmitHsts(true, "https");
    const bool b = shouldEmitHsts(true, "https");
    EXPECT_EQ(a, b);
    const bool c = shouldEmitHsts(false, "http");
    const bool d = shouldEmitHsts(false, "http");
    EXPECT_EQ(c, d);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
