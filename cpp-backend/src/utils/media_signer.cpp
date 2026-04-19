#include "utils/media_signer.h"

#include "utils/config.h"
#include "utils/logger.h"

#include <openssl/hmac.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace vms::utils {

static std::string getMediaSigningSecret() {
    if (const char* env = std::getenv("VMS_MEDIA_SIGNING_SECRET"); env && *env) {
        return std::string(env);
    }
    // Fallback: auth secret (still better than hardcoded)
    return vms::Config::getInstance().getAuthConfig().secret_key;
}

static std::string hmacSha256Bin(const std::string& key, const std::string& data) {
    unsigned int len = 0;
    unsigned char out[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         out, &len);
    return std::string(reinterpret_cast<char*>(out), len);
}

static std::string toHex(const std::string& bin) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char c : bin) oss << std::setw(2) << static_cast<int>(c);
    return oss.str();
}

static bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

static std::string sanitizeScopePart(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.') {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static std::string signingPayload(const std::string& path, long long exp, const MediaAccessScope& scope) {
    std::ostringstream oss;
    oss << path << "|" << exp << "|"
        << sanitizeScopePart(scope.scope) << "|";
    if (scope.camera_id >= 0) {
        oss << scope.camera_id;
    }
    oss << "|"
        << sanitizeScopePart(scope.resource_id) << "|"
        << sanitizeScopePart(scope.role);
    return oss.str();
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static bool endsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

static bool verifyScopeMatchesPath(const std::string& path, const MediaAccessScope& scope) {
    if (scope.scope.empty()) {
        return true; // backward compatible with legacy links without scope
    }

    if (scope.scope == "snapshot") {
        return startsWith(path, "/api/snapshots/files/");
    }
    if (scope.scope == "event_video") {
        const std::string prefix = "/api/events/";
        if (!startsWith(path, prefix) || !endsWith(path, "/video")) return false;
        if (!scope.resource_id.empty()) {
            return path == (prefix + scope.resource_id + "/video");
        }
        return true;
    }
    if (scope.scope == "recording_video") {
        const std::string prefix = "/api/recordings/";
        if (!startsWith(path, prefix) || !endsWith(path, "/video")) return false;
        if (!scope.resource_id.empty()) {
            return path == (prefix + scope.resource_id + "/video");
        }
        return true;
    }
    if (scope.scope == "recording_segment_video") {
        const std::string prefix = "/api/recordings/segments/";
        if (!startsWith(path, prefix) || !endsWith(path, "/video")) return false;
        if (!scope.resource_id.empty()) {
            return path == (prefix + scope.resource_id + "/video");
        }
        return true;
    }
    if (scope.scope == "camera_frame") {
        if (!startsWith(path, "/api/cameras/") || path.find("/frame") == std::string::npos) return false;
        if (scope.camera_id >= 0) {
            const std::string id_marker = "/api/cameras/" + std::to_string(scope.camera_id) + "/";
            return path.find(id_marker) == 0;
        }
        return true;
    }
    if (scope.scope == "storage") {
        return startsWith(path, "/api/storage/");
    }
    return false;
}

static int getMaxPresignTtl() {
    const int cfg = vms::Config::getInstance().getMediaSigningConfig().max_ttl_seconds;
    return (cfg > 0 && cfg <= 86400) ? cfg : 900; // hard-clamp: never exceed 24h
}

std::string presignPath(const std::string& path, int ttl_seconds, const MediaAccessScope& scope) {
    if (path.empty()) return path;

    const auto now = std::chrono::system_clock::now();
    const long long now_s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const int max_ttl = getMaxPresignTtl();
    // C6: enforce TTL ceiling — caller cannot create URLs valid beyond configured max
    const long long capped_ttl = std::max(1, std::min(ttl_seconds, max_ttl));
    const long long exp = now_s + capped_ttl;

    const std::string payload = signingPayload(path, exp, scope);
    const std::string sig = toHex(hmacSha256Bin(getMediaSigningSecret(), payload));

    // append query (path is already a pure path, not including query)
    std::ostringstream oss;
    oss << path << "?exp=" << exp << "&sig=" << sig;
    if (!scope.scope.empty()) oss << "&scope=" << sanitizeScopePart(scope.scope);
    if (scope.camera_id >= 0) oss << "&camera_id=" << scope.camera_id;
    if (!scope.resource_id.empty()) oss << "&resource_id=" << sanitizeScopePart(scope.resource_id);
    if (!scope.role.empty()) oss << "&role=" << sanitizeScopePart(scope.role);
    return oss.str();
}

bool verifyPresign(const std::string& path,
                   long long exp_unix_seconds,
                   const std::string& sig_hex,
                   const MediaAccessScope& scope) {
    if (path.empty() || sig_hex.empty() || exp_unix_seconds <= 0) return false;

    const auto now = std::chrono::system_clock::now();
    const long long now_s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    if (now_s > exp_unix_seconds) return false;

    // C6: Reject URLs whose remaining TTL exceeds the cap
    if ((exp_unix_seconds - now_s) > getMaxPresignTtl()) return false;

    if (!verifyScopeMatchesPath(path, scope)) return false;

    const std::string payload = signingPayload(path, exp_unix_seconds, scope);
    const std::string expected = toHex(hmacSha256Bin(getMediaSigningSecret(), payload));
    if (constantTimeEquals(sig_hex, expected)) {
        return true;
    }

    // M8: Legacy unscoped fallback (path|exp only) is disabled by default.
    // Enable only during a migration window with media_signing.allow_legacy_unsigned=true in config.
    const bool legacy_allowed =
        vms::Config::getInstance().getMediaSigningConfig().allow_legacy_unsigned;
    if (legacy_allowed &&
        scope.scope.empty() && scope.camera_id < 0 && scope.resource_id.empty() && scope.role.empty()) {
        LOG_THROTTLED_WARN(10000, "Verifying legacy unscoped presign URL — migrate to scoped URLs and "
                           "set media_signing.allow_legacy_unsigned=false");
        const std::string legacy_payload = path + "|" + std::to_string(exp_unix_seconds);
        const std::string legacy_expected = toHex(hmacSha256Bin(getMediaSigningSecret(), legacy_payload));
        return constantTimeEquals(sig_hex, legacy_expected);
    }
    return false;
}

} // namespace vms::utils

