#include <gtest/gtest.h>
#include <string>

// Now links the REAL formatter from `utils/session_cookie.h` instead of an
// inline copy. Previously this file reproduced the format-string locally —
// which meant editing user_controller.cpp could leave the test green but
// production broken. The split is: formatSessionCookie is a pure header
// function (token, ttl, secure → string); user_controller.cpp wraps it
// with Config-backed TTL and X-Forwarded-Proto detection.

#include "utils/session_cookie.h"

using vms::utils::formatSessionCookie;

TEST(SessionCookie, ContainsHttpOnly) {
    auto cookie = formatSessionCookie("tok123", 3600, false);
    EXPECT_NE(cookie.find("HttpOnly"), std::string::npos);
}

TEST(SessionCookie, ContainsSameSiteStrict) {
    auto cookie = formatSessionCookie("tok123", 3600, false);
    EXPECT_NE(cookie.find("SameSite=Strict"), std::string::npos);
}

TEST(SessionCookie, ContainsPathScope) {
    auto cookie = formatSessionCookie("tok123", 3600, false);
    // Path=/api confines the cookie to API requests — protects /static/* and
    // any third-party content served from the same origin.
    EXPECT_NE(cookie.find("Path=/api"), std::string::npos);
}

TEST(SessionCookie, SecureFlagPresent) {
    auto cookie = formatSessionCookie("tok123", 3600, true);
    EXPECT_NE(cookie.find("; Secure"), std::string::npos);
}

TEST(SessionCookie, SecureFlagAbsent) {
    auto cookie = formatSessionCookie("tok123", 3600, false);
    EXPECT_EQ(cookie.find("; Secure"), std::string::npos);
}

TEST(SessionCookie, LogoutCookieExpiresImmediately) {
    // Logout reuses the same formatter with ttl=0; the Max-Age=0 ask is what
    // tells the browser to drop the cookie. Regression guard for an old bug
    // where logout produced Max-Age=1 and the cookie survived for 1 second
    // post-logout.
    auto cookie = formatSessionCookie("", 0, false);
    EXPECT_NE(cookie.find("Max-Age=0"), std::string::npos);
    EXPECT_EQ(cookie.find("Max-Age=1"), std::string::npos);
}

TEST(SessionCookie, TokenEmbedded) {
    auto cookie = formatSessionCookie("mytoken42", 1800, false);
    EXPECT_NE(cookie.find("vms_session=mytoken42"), std::string::npos);
}

TEST(SessionCookie, TtlReflectedInMaxAge) {
    auto cookie = formatSessionCookie("t", 1800, false);
    EXPECT_NE(cookie.find("Max-Age=1800"), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
