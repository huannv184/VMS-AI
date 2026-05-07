#pragma once

#include <string>

namespace vms {
namespace utils {

/**
 * @brief Simple encryption utility for protecting sensitive data at rest (e.g. 2FA secrets).
 * Uses AES-256-CBC via OpenSSL.
 */
class Crypto {
public:
    /**
     * @brief Encrypts data using a key derived from the system secret.
     * Returns base64 encoded string: "v1:<iv_base64>:<ciphertext_base64>"
     */
    static std::string encrypt(const std::string& plaintext);

    /**
     * @brief Decrypts data encrypted with encrypt().
     */
    static std::string decrypt(const std::string& ciphertext_envelope);

private:
    static std::string getSystemKey();
};

} // namespace utils
} // namespace vms
