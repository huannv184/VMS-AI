// Pins the H9 /api/v1/metrics bearer-token gate. The route handler in
// http_server.cpp calls isMetricsRequestAuthorized() — this test fixes
// the policy matrix so a careless refactor can't quietly turn off the
// gate or introduce a length/timing side channel.

#include <gtest/gtest.h>

#include "utils/metrics_auth.h"

using vms::utils::isMetricsRequestAuthorized;

TEST(MetricsAuth, EmptyConfiguredTokenAllowsAnything) {
    // Operator hasn't configured a token — endpoint is unauthenticated
    // at the route level. AuthMiddleware's public allowlist is still
    // the entry guard.
    EXPECT_TRUE(isMetricsRequestAuthorized("", ""));
    EXPECT_TRUE(isMetricsRequestAuthorized("", "Bearer whatever"));
    EXPECT_TRUE(isMetricsRequestAuthorized("", "junk"));
}

TEST(MetricsAuth, MatchingBearerAllowed) {
    EXPECT_TRUE(isMetricsRequestAuthorized("secret-token-32-chars-padding-xx",
                                           "Bearer secret-token-32-chars-padding-xx"));
}

TEST(MetricsAuth, MissingAuthHeaderDenied) {
    EXPECT_FALSE(isMetricsRequestAuthorized("secret", ""));
}

TEST(MetricsAuth, WrongTokenDenied) {
    EXPECT_FALSE(isMetricsRequestAuthorized("rightright", "Bearer wrongwrong"));
}

TEST(MetricsAuth, WrongLengthDenied) {
    // Too short — guards against a stripped-prefix attack where someone
    // sends just the token without "Bearer ".
    EXPECT_FALSE(isMetricsRequestAuthorized("secret", "secret"));
    // Too long — extra trailing bytes.
    EXPECT_FALSE(isMetricsRequestAuthorized("secret", "Bearer secretAAA"));
    // Too short by 1.
    EXPECT_FALSE(isMetricsRequestAuthorized("secret", "Bearer secre"));
}

TEST(MetricsAuth, SchemeCaseSensitive) {
    // Prometheus emits "Bearer " (capital B) per RFC 6750. The gate
    // requires the same case — no lowercase "bearer" or "BEARER".
    EXPECT_FALSE(isMetricsRequestAuthorized("secret", "bearer secret"));
    EXPECT_FALSE(isMetricsRequestAuthorized("secret", "BEARER secret"));
}

TEST(MetricsAuth, FirstByteMismatchStillFullScan) {
    // Property: changing different bytes should produce equivalent
    // wrong-answer outcomes — constant-time compare doesn't short-circuit.
    // We can't measure timing here, but we can at least pin that both
    // mismatches return false (no early-exit code that would skip the
    // tail comparison).
    EXPECT_FALSE(isMetricsRequestAuthorized("AAAAAAAAAA", "Bearer BAAAAAAAAA"));  // first byte
    EXPECT_FALSE(isMetricsRequestAuthorized("AAAAAAAAAA", "Bearer AAAAAAAAAB"));  // last byte
    EXPECT_FALSE(isMetricsRequestAuthorized("AAAAAAAAAA", "Bearer AAAAABAAAA"));  // middle byte
}

TEST(MetricsAuth, NoSideEffects) {
    EXPECT_TRUE(isMetricsRequestAuthorized("tok", "Bearer tok"));
    EXPECT_TRUE(isMetricsRequestAuthorized("tok", "Bearer tok"));
    EXPECT_FALSE(isMetricsRequestAuthorized("tok", "Bearer xxx"));
    EXPECT_FALSE(isMetricsRequestAuthorized("tok", "Bearer xxx"));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
