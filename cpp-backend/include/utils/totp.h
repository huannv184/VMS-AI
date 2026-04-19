#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace vms {
namespace utils {

class TOTP {
public:
    /**
     * @brief Generate a random 16-character Base32 secret
     */
    static std::string generateSecret();

    /**
     * @brief Validate a 6-digit TOTP code against a secret
     * @param secret Base32 encoded secret
     * @param code The 6-digit code to verify
     * @param window Time window (default 1 = ±30s)
     */
    static bool verifyCode(const std::string& secret, const std::string& code, int window = 1);

    /**
     * @brief Get a QR code provisioning URI (otpauth://...)
     */
    static std::string getProvisioningUri(const std::string& username, const std::string& secret, const std::string& issuer = "VMS_Enterprise");

private:
    static std::vector<uint8_t> base32Decode(const std::string& input);
    static uint32_t truncate(const std::vector<uint8_t>& hmac);
};

} // namespace utils
} // namespace vms
