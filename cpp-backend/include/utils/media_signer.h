#pragma once

#include <string>

namespace vms::utils {

struct MediaAccessScope {
    std::string scope;      // e.g. snapshot, recording_video, event_video, storage
    int camera_id = -1;     // optional constraint
    std::string resource_id; // optional event/recording/segment id
    std::string role;       // optional role hint/constraint
};

/**
 * Presigned URL scheme for media endpoints (snapshots/videos/storage).
 *
 * Query params:
 *  - exp: unix timestamp (seconds)
 *  - sig: hex(HMAC_SHA256(secret, path + "|" + exp))
 *
 * The `path` must be the request path WITHOUT query string,
 * e.g. "/api/snapshots/files/foo.jpg".
 */
std::string presignPath(const std::string& path, int ttl_seconds = 120, const MediaAccessScope& scope = {});

bool verifyPresign(const std::string& path,
                   long long exp_unix_seconds,
                   const std::string& sig_hex,
                   const MediaAccessScope& scope = {});

} // namespace vms::utils

