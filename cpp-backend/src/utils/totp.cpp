#include "utils/totp.h"
#include "utils/sha1.h"
#include <ctime>
#include <algorithm>
#include <random>
#include <cmath>

namespace vms {
namespace utils {

std::string TOTP::generateSecret() {
    static const char* base32_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 31);
    std::string secret = "";
    for (int i = 0; i < 16; i++) {
        secret += base32_chars[dist(rd)];
    }
    return secret;
}

bool TOTP::verifyCode(const std::string& secret, const std::string& code, int window) {
    if (code.size() != 6 || secret.empty()) return false;
    
    std::vector<uint8_t> key = base32Decode(secret);
    if (key.empty()) return false;

    long long current_time = std::time(nullptr) / 30; // 30 second step
    
    for (int i = -window; i <= window; i++) {
        long long t = current_time + i;
        std::vector<uint8_t> msg(8);
        for (int j = 7; j >= 0; j--) {
            msg[j] = static_cast<uint8_t>(t & 0xFF);
            t >>= 8;
        }

        std::vector<uint8_t> hmac = SHA1::hmac(key, msg);
        uint32_t val = truncate(hmac);
        uint32_t truncated_code = val % 1000000;
        
        std::string generated_code = std::to_string(truncated_code);
        while (generated_code.size() < 6) generated_code = "0" + generated_code;
        
        if (generated_code == code) return true;
    }
    
    return false;
}

std::string TOTP::getProvisioningUri(const std::string& username, const std::string& secret, const std::string& issuer) {
    return "otpauth://totp/" + issuer + ":" + username + "?secret=" + secret + "&issuer=" + issuer;
}

std::vector<uint8_t> TOTP::base32Decode(const std::string& input) {
    std::vector<uint8_t> result;
    uint32_t buffer = 0;
    int bits_left = 0;

    for (char c : input) {
        int val = 0;
        if (c >= 'A' && c <= 'Z') val = c - 'A';
        else if (c >= 'a' && c <= 'z') val = c - 'a';
        else if (c >= '2' && c <= '7') val = c - '2' + 26;
        else if (c == '=') break; // Padding
        else continue; // Ignore other chars

        buffer = (buffer << 5) | val;
        bits_left += 5;
        if (bits_left >= 8) {
            result.push_back(static_cast<uint8_t>((buffer >> (bits_left - 8)) & 0xFF));
            bits_left -= 8;
        }
    }
    return result;
}

uint32_t TOTP::truncate(const std::vector<uint8_t>& hmac) {
    int offset = hmac[19] & 0x0F;
    return ((hmac[offset] & 0x7F) << 24) |
           ((hmac[offset + 1] & 0xFF) << 16) |
           ((hmac[offset + 2] & 0xFF) << 8) |
           (hmac[offset + 3] & 0xFF);
}

} // namespace utils
} // namespace vms
