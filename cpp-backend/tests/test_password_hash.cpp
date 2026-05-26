// Verifies the argon2id wiring activated by VMS_HAS_ARGON2 in CMakeLists.txt.
// If this build target ever loses the compile definition or the libargon2
// link, the test below either fails to link (no argon2 symbol) or throws at
// runtime ("Argon2 library not compiled in") — both surface the regression
// before the silent SHA256 fallback ships to production.

#include <gtest/gtest.h>
#include <string>
#include <stdexcept>

#include "utils/password_hash.h"
#include "utils/sha256.h"

using vms::utils::hashPasswordArgon2;
using vms::utils::verifyPasswordArgon2;
using vms::utils::isArgon2Hash;
using vms::utils::generateArgon2Salt;
using vms::utils::SHA256;

TEST(PasswordHash, GenerateSaltIsHexAndCorrectLength) {
    const std::string salt = generateArgon2Salt();
    // kArgon2SaltLen = 16 bytes → 32 hex chars
    EXPECT_EQ(salt.size(), 32u);
    for (char c : salt) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-hex char in salt: " << c;
    }
}

TEST(PasswordHash, HashHasArgon2idPrefixAndParams) {
    const std::string salt = generateArgon2Salt();
    const std::string h = hashPasswordArgon2("secret-pw", salt);
    // Format guard — operators grep logs / DB for this exact prefix.
    EXPECT_EQ(h.rfind("$argon2id$v=19$", 0), 0u);
    EXPECT_NE(h.find("m=65536"), std::string::npos);
    EXPECT_NE(h.find("t=3"), std::string::npos);
    EXPECT_NE(h.find("p=4"), std::string::npos);
    EXPECT_NE(h.find(salt), std::string::npos);
}

TEST(PasswordHash, IsArgon2HashRecognizesOwnFormat) {
    const std::string h = hashPasswordArgon2("x", generateArgon2Salt());
    EXPECT_TRUE(isArgon2Hash(h));
}

TEST(PasswordHash, IsArgon2HashRejectsSha256Hex) {
    // Legacy SHA256 hex output is 64 chars, no $ prefix.
    const std::string legacy = SHA256::hash("password");
    EXPECT_FALSE(isArgon2Hash(legacy));
}

TEST(PasswordHash, IsArgon2HashRejectsEmptyAndJunk) {
    EXPECT_FALSE(isArgon2Hash(""));
    EXPECT_FALSE(isArgon2Hash("argon2id"));            // missing leading $
    EXPECT_FALSE(isArgon2Hash("$argon2i$v=19$..."));   // wrong variant (i, not id)
}

TEST(PasswordHash, VerifyRoundTrip) {
    const std::string pw = "correct horse battery staple";
    const std::string salt = generateArgon2Salt();
    const std::string h = hashPasswordArgon2(pw, salt);
    EXPECT_TRUE(verifyPasswordArgon2(pw, h));
}

TEST(PasswordHash, VerifyRejectsWrongPassword) {
    const std::string salt = generateArgon2Salt();
    const std::string h = hashPasswordArgon2("right-pw", salt);
    EXPECT_FALSE(verifyPasswordArgon2("wrong-pw", h));
}

TEST(PasswordHash, VerifyRejectsTamperedHash) {
    const std::string salt = generateArgon2Salt();
    std::string h = hashPasswordArgon2("pw", salt);
    // Flip the last hex char of the embedded hash — constant-time compare
    // should still drop the verification.
    h.back() = (h.back() == 'a') ? 'b' : 'a';
    EXPECT_FALSE(verifyPasswordArgon2("pw", h));
}

TEST(PasswordHash, VerifyRejectsNonArgon2Input) {
    // Legacy SHA256 hash must be rejected by the argon2-only verifier so the
    // caller is forced down the explicit legacy-fallback path in user_controller.
    const std::string legacy = SHA256::hash("pw");
    EXPECT_FALSE(verifyPasswordArgon2("pw", legacy));
}

TEST(PasswordHash, HashRejectsBadSaltLength) {
    EXPECT_THROW(hashPasswordArgon2("pw", "deadbeef"), std::invalid_argument);  // 8 chars, want 32
    EXPECT_THROW(hashPasswordArgon2("pw", ""),         std::invalid_argument);
}

TEST(PasswordHash, DistinctSaltsProduceDistinctHashes) {
    const std::string pw = "same-password";
    const std::string h1 = hashPasswordArgon2(pw, generateArgon2Salt());
    const std::string h2 = hashPasswordArgon2(pw, generateArgon2Salt());
    EXPECT_NE(h1, h2);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
