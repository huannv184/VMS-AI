// SEC-002 regression: verify cross-purpose token isolation in jwt_utils.cpp.
//
// The default-password gate depends on a single load-bearing property of
// jwt_utils: a token signed with token_use="password_change_pending" must NOT
// pass verifyAccessTokenJwt(), and a token signed with token_use="access" must
// NOT pass verifyPasswordChangeTempTokenJwt(). If that isolation breaks, an
// admin/admin login can hand a temp token back into AuthMiddleware and get
// full access — which is exactly the bypass we just fixed.
//
// Unlike test_auth.cpp (which inline-reproduces cookie strings), this links
// the REAL jwt_utils.cpp / config.cpp so the test is sensitive to refactors
// of the JWT signing/verifying code.

#include <gtest/gtest.h>
#include "utils/jwt_utils.h"
#include "core/user.h"

namespace {

vms::core::User makeAdmin() {
    vms::core::User u;
    u.id = 1;
    u.username = "admin";
    u.role_id = 1;
    u.role_name = "admin";
    u.is_active = true;
    u.two_factor_enabled = false;
    u.token_version = 0;
    return u;
}

}  // namespace

TEST(JwtPurpose, PasswordChangeTokenRejectedByAccessVerifier) {
    auto user = makeAdmin();
    const std::string temp = vms::utils::createPasswordChangeTempTokenJwt(user);
    ASSERT_FALSE(temp.empty());

    int uid = -1;
    EXPECT_FALSE(vms::utils::verifyAccessTokenJwt(temp, uid))
        << "password_change_pending token must NOT satisfy the access verifier "
           "— that bypass is exactly SEC-002.";
}

TEST(JwtPurpose, PasswordChangeTokenAcceptedByOwnVerifier) {
    auto user = makeAdmin();
    const std::string temp = vms::utils::createPasswordChangeTempTokenJwt(user);
    int uid = -1;
    ASSERT_TRUE(vms::utils::verifyPasswordChangeTempTokenJwt(temp, uid));
    EXPECT_EQ(uid, user.id);
}

TEST(JwtPurpose, AccessTokenRejectedByPasswordChangeVerifier) {
    auto user = makeAdmin();
    const std::string access = vms::utils::createAccessTokenJwt(user);
    int uid = -1;
    EXPECT_FALSE(vms::utils::verifyPasswordChangeTempTokenJwt(access, uid))
        << "Real access tokens must not be accepted by the password-change "
           "endpoint — would let any authenticated user rotate the admin pwd.";
}

TEST(JwtPurpose, TwoFactorTokenRejectedByPasswordChangeVerifier) {
    auto user = makeAdmin();
    const std::string twofa = vms::utils::createTwoFactorTempTokenJwt(user);
    int uid = -1;
    EXPECT_FALSE(vms::utils::verifyPasswordChangeTempTokenJwt(twofa, uid))
        << "Cross-purpose isolation: 2fa_pending != password_change_pending.";
}

TEST(JwtPurpose, GarbageTokenRejected) {
    int uid = -1;
    EXPECT_FALSE(vms::utils::verifyPasswordChangeTempTokenJwt("not.a.jwt", uid));
    EXPECT_FALSE(vms::utils::verifyAccessTokenJwt("not.a.jwt", uid));
    EXPECT_FALSE(vms::utils::verifyTwoFactorTempTokenJwt("not.a.jwt", uid));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
