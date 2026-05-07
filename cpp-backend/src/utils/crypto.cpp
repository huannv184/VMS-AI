#include "utils/crypto.h"
#include "utils/config.h"
#include "utils/logger.h"
#include "utils/api_utils.h"
#include "utils/sha256.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <vector>
#include <sstream>
#include <iomanip>

namespace vms {
namespace utils {

namespace {

void handleErrors() {
    unsigned long errCode;
    while ((errCode = ERR_get_error()) != 0) {
        char* err = ERR_error_string(errCode, NULL);
        LOG_ERROR("OpenSSL Error: {}", err);
    }
}

} // namespace

std::string Crypto::getSystemKey() {
    // Derive a 32-byte (256-bit) key from the system secret key
    const std::string& secret = vms::Config::getInstance().getAuthConfig().secret_key;
    std::string h = vms::utils::SHA256::hash(secret + "vms-salt-2026");
    // SHA256 returns 64 hex chars = 32 bytes of entropy
    // We'll decode the hex back to binary
    std::string binary_key;
    for (size_t i = 0; i < h.length(); i += 2) {
        std::string part = h.substr(i, 2);
        char ch = (char)std::stoul(part, nullptr, 16);
        binary_key.push_back(ch);
    }
    return binary_key;
}

std::string Crypto::encrypt(const std::string& plaintext) {
    if (plaintext.empty()) return "";

    std::string key = getSystemKey();
    unsigned char iv[16];
    if (RAND_bytes(iv, sizeof(iv)) != 1) {
        LOG_ERROR("Failed to generate random IV");
        return "";
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, 
                           reinterpret_cast<const unsigned char*>(key.data()), iv) != 1) {
        handleErrors();
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }

    std::vector<unsigned char> ciphertext(plaintext.size() + 16); // +16 for padding
    int len = 0;
    int ciphertext_len = 0;

    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, 
                          reinterpret_cast<const unsigned char*>(plaintext.data()), 
                          static_cast<int>(plaintext.size())) != 1) {
        handleErrors();
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        handleErrors();
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    ciphertext_len += len;

    EVP_CIPHER_CTX_free(ctx);

    // Envelope: "v1:<iv_b64>:<ciphertext_b64>"
    std::string iv_b64 = vms::api::ApiUtils::base64_encode(iv, sizeof(iv));
    std::string cipher_b64 = vms::api::ApiUtils::base64_encode(ciphertext.data(), ciphertext_len);

    return "v1:" + iv_b64 + ":" + cipher_b64;
}

std::string Crypto::decrypt(const std::string& envelope) {
    if (envelope.empty()) return "";

    // BUG-20 FIX: Was `if (no v1: prefix) return envelope` — silent plaintext
    // fallback let an attacker who could write any value into the encrypted
    // column (SQL injection, compromised admin) supply a known plaintext that
    // bypassed the encryption check entirely. For 2FA secrets that means
    // forging valid TOTP codes for any user.
    //
    // Now: opt-in via VMS_CRYPTO_ALLOW_LEGACY_PLAINTEXT=1 for one-shot
    // migration runs only. Default: refuse non-prefixed values so corrupted /
    // tampered entries surface as decrypt failures instead of being trusted.
    if (envelope.substr(0, 3) != "v1:") {
        static const bool allow_legacy = []() {
            const char* e = std::getenv("VMS_CRYPTO_ALLOW_LEGACY_PLAINTEXT");
            return e && *e && std::atoi(e) != 0;
        }();
        if (allow_legacy) {
            LOG_WARN("Crypto::decrypt: accepting legacy plaintext value "
                     "(VMS_CRYPTO_ALLOW_LEGACY_PLAINTEXT=1). Re-encrypt and "
                     "remove this env var after migration.");
            return envelope;
        }
        LOG_WARN("Crypto::decrypt: rejected value without v1: envelope. "
                 "Set VMS_CRYPTO_ALLOW_LEGACY_PLAINTEXT=1 only for one-shot migration.");
        return "";
    }

    std::stringstream ss(envelope.substr(3));
    std::string iv_b64, cipher_b64;
    std::getline(ss, iv_b64, ':');
    std::getline(ss, cipher_b64, ':');

    if (iv_b64.empty() || cipher_b64.empty()) return "";

    std::string iv_raw, cipher_raw;
    try {
        iv_raw = vms::api::ApiUtils::base64_decode(iv_b64);
        cipher_raw = vms::api::ApiUtils::base64_decode(cipher_b64);
    } catch (...) {
        return "";
    }

    if (iv_raw.size() != 16) return "";

    std::string key = getSystemKey();
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, 
                           reinterpret_cast<const unsigned char*>(key.data()), 
                           reinterpret_cast<const unsigned char*>(iv_raw.data())) != 1) {
        handleErrors();
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }

    std::vector<unsigned char> plaintext(cipher_raw.size());
    int len = 0;
    int plaintext_len = 0;

    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, 
                          reinterpret_cast<const unsigned char*>(cipher_raw.data()), 
                          static_cast<int>(cipher_raw.size())) != 1) {
        handleErrors();
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    plaintext_len = len;

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        // This might happen if key is wrong or data is corrupted
        // LOG_DEBUG because it might be common during key rotation/migration
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return std::string(reinterpret_cast<char*>(plaintext.data()), plaintext_len);
}

} // namespace utils
} // namespace vms
