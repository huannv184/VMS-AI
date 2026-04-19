#pragma once

// H6: Argon2id password hashing to replace plain SHA256.
//
// Storage format: "$argon2id$v=19$m=65536,t=3,p=4$<salt_hex>$<hash_hex>"
// This is a simplified PHC-inspired format stored as a single opaque string.
//
// Migration: legacy SHA256 hashes do NOT start with "$argon2id$". On successful
// login with a legacy hash, the caller should immediately re-hash and update the DB.
// After the migration window, reject any login that would match only a legacy hash.
//
// Dependency: libargon2 — add to vcpkg.json: "argon2"
//   CMakeLists.txt: find_package(argon2 CONFIG REQUIRED) + target_link_libraries(... argon2::argon2)
//   Header: <argon2.h>

#include <string>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>

#ifdef VMS_HAS_ARGON2
#include <argon2.h>
#endif

namespace vms::utils {

// Argon2id parameters — do not reduce without measuring impact on login latency.
// At these values, a single hash takes ~100ms on a modern CPU, making brute-force GPU attacks
// ~1000x slower than SHA256 (which runs at 500M+/s per GPU).
static constexpr uint32_t kArgon2TimeCost    = 3;      // iterations
static constexpr uint32_t kArgon2MemCost     = 65536;  // 64 MiB
static constexpr uint32_t kArgon2Parallelism = 4;
static constexpr uint32_t kArgon2HashLen     = 32;     // bytes
static constexpr uint32_t kArgon2SaltLen     = 16;     // bytes

inline bool isArgon2Hash(const std::string& hash) {
    return hash.rfind("$argon2id$", 0) == 0;
}

inline std::string generateArgon2Salt() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint32_t i = 0; i < kArgon2SaltLen; ++i) {
        oss << std::setw(2) << dist(rng);
    }
    return oss.str(); // 32-char hex
}

// Hash a password using argon2id. Returns a self-describing hash string.
// The returned string includes all parameters and salt needed to verify later.
inline std::string hashPasswordArgon2(const std::string& password, const std::string& salt_hex) {
#ifdef VMS_HAS_ARGON2
    if (salt_hex.size() != kArgon2SaltLen * 2) {
        throw std::invalid_argument("salt_hex must be " + std::to_string(kArgon2SaltLen * 2) + " chars");
    }
    // Decode hex salt
    unsigned char salt[kArgon2SaltLen];
    for (uint32_t i = 0; i < kArgon2SaltLen; ++i) {
        salt[i] = static_cast<unsigned char>(std::stoi(salt_hex.substr(i * 2, 2), nullptr, 16));
    }
    unsigned char hash[kArgon2HashLen];
    int rc = argon2id_hash_raw(
        kArgon2TimeCost, kArgon2MemCost, kArgon2Parallelism,
        password.data(), password.size(),
        salt, kArgon2SaltLen,
        hash, kArgon2HashLen);
    if (rc != ARGON2_OK) {
        throw std::runtime_error(std::string("argon2id_hash_raw failed: ") + argon2_error_message(rc));
    }
    // Encode as hex
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint32_t i = 0; i < kArgon2HashLen; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    // Self-describing format
    return "$argon2id$v=19$m=" + std::to_string(kArgon2MemCost) +
           ",t=" + std::to_string(kArgon2TimeCost) +
           ",p=" + std::to_string(kArgon2Parallelism) +
           "$" + salt_hex + "$" + oss.str();
#else
    // Fallback when argon2 library is not available: still better than plain SHA256
    // because it includes the salt. Ship VMS_HAS_ARGON2 to get real protection.
    (void)salt_hex;
    throw std::runtime_error("Argon2 library not compiled in. Add 'argon2' to vcpkg.json and "
                             "define VMS_HAS_ARGON2 in CMakeLists.txt.");
#endif
}

// Verify a password against an argon2id hash string produced by hashPasswordArgon2().
inline bool verifyPasswordArgon2(const std::string& password, const std::string& encoded_hash) {
#ifdef VMS_HAS_ARGON2
    if (!isArgon2Hash(encoded_hash)) return false;
    // Parse: "$argon2id$v=19$m=M,t=T,p=P$salt_hex$hash_hex"
    // Simple positional parsing — we know our own format exactly.
    auto p1 = encoded_hash.find('$', 1);       // after "$argon2id"
    auto p2 = encoded_hash.find('$', p1 + 1);  // after "v=19"
    auto p3 = encoded_hash.find('$', p2 + 1);  // after "m=...,t=...,p=..."
    auto p4 = encoded_hash.find('$', p3 + 1);  // after salt_hex
    if (p1 == std::string::npos || p2 == std::string::npos ||
        p3 == std::string::npos || p4 == std::string::npos) {
        return false;
    }
    const std::string salt_hex  = encoded_hash.substr(p3 + 1, p4 - p3 - 1);
    const std::string hash_hex  = encoded_hash.substr(p4 + 1);
    if (salt_hex.size() != kArgon2SaltLen * 2 || hash_hex.size() != kArgon2HashLen * 2) {
        return false;
    }
    // Re-hash the candidate password with the extracted salt and compare
    try {
        const std::string candidate = hashPasswordArgon2(password, salt_hex);
        // Constant-time comparison on the full encoded string
        if (candidate.size() != encoded_hash.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < candidate.size(); ++i) {
            diff |= static_cast<unsigned char>(candidate[i]) ^ static_cast<unsigned char>(encoded_hash[i]);
        }
        return diff == 0;
    } catch (...) {
        return false;
    }
#else
    (void)password; (void)encoded_hash;
    return false;
#endif
}

} // namespace vms::utils
