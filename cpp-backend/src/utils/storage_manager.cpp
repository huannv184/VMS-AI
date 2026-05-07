#include "utils/storage_manager.h"
#include "utils/logger.h"
#include "utils/config.h"
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <fstream>
#include <vector>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <thread>

namespace vms {
namespace utils {

// ============================================================================
// Helper: SHA256 hash as hex string
// ============================================================================
static std::string sha256Hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    return oss.str();
}

// ============================================================================
// Helper: HMAC-SHA256 (binary)
// ============================================================================
static std::string hmacSha256(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         result, &len);
    return std::string(reinterpret_cast<char*>(result), len);
}

// ============================================================================
// Helper: Get current UTC time strings
// ============================================================================
static void getUtcTime(std::string& dateStamp, std::string& amzDate) {
    time_t now = time(nullptr);
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char date_buf[16], amz_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y%m%d", &utc);
    strftime(amz_buf, sizeof(amz_buf), "%Y%m%dT%H%M%SZ", &utc);
    dateStamp = date_buf;
    amzDate = amz_buf;
}

// ============================================================================
// Helper: Parse host from endpoint URL
// ============================================================================
static std::string parseHost(const std::string& endpoint) {
    // "http://localhost:9000" -> "localhost:9000"
    auto pos = endpoint.find("://");
    if (pos != std::string::npos) return endpoint.substr(pos + 3);
    return endpoint;
}

// ============================================================================
// AWS Signature V4 — generates Authorization header for S3/MinIO
// ============================================================================
struct S3AuthHeaders {
    std::string authorization;
    std::string amz_date;
    std::string content_sha256;
};

static S3AuthHeaders signV4(const std::string& method,
                            const std::string& uri,
                            const std::string& host,
                            const std::string& access_key,
                            const std::string& secret_key,
                            const std::string& payload_hash = "UNSIGNED-PAYLOAD",
                            const std::string& region = "us-east-1",
                            const std::string& service = "s3") {
    S3AuthHeaders result;
    std::string dateStamp, amzDate;
    getUtcTime(dateStamp, amzDate);
    result.amz_date = amzDate;
    result.content_sha256 = payload_hash;

    // 1. Canonical Request
    std::string canonical_uri = uri.empty() ? "/" : uri;
    std::string canonical_querystring;  // empty for our simple calls
    std::string canonical_headers =
        "host:" + host + "\n"
        "x-amz-content-sha256:" + payload_hash + "\n"
        "x-amz-date:" + amzDate + "\n";
    std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
    std::string canonical_request =
        method + "\n" + canonical_uri + "\n" + canonical_querystring + "\n" +
        canonical_headers + "\n" + signed_headers + "\n" + payload_hash;

    // 2. String to Sign
    std::string scope = dateStamp + "/" + region + "/" + service + "/aws4_request";
    std::string string_to_sign =
        "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + sha256Hex(canonical_request);

    // 3. Signing Key
    std::string kDate    = hmacSha256("AWS4" + secret_key, dateStamp);
    std::string kRegion  = hmacSha256(kDate, region);
    std::string kService = hmacSha256(kRegion, service);
    std::string kSigning = hmacSha256(kService, "aws4_request");

    // 4. Signature (hex)
    std::string signature_bin = hmacSha256(kSigning, string_to_sign);
    std::ostringstream sig_hex;
    for (unsigned char c : signature_bin)
        sig_hex << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)c;

    // 5. Authorization header
    result.authorization =
        "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
        ", SignedHeaders=" + signed_headers +
        ", Signature=" + sig_hex.str();

    return result;
}

// ============================================================================
// CURL write callback (for responses)
// ============================================================================
static size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* vec = static_cast<std::vector<char>*>(userdata);
    size_t total = size * nmemb;
    vec->insert(vec->end(), ptr, ptr + total);
    return total;
}

// ============================================================================
// StorageManager Implementation
// ============================================================================
StorageManager& StorageManager::getInstance() {
    static StorageManager instance;
    return instance;
}

bool StorageManager::init(const Config::StorageConfig& config) {
    driver_ = config.driver;
    config_ = config.minio;
    initialized_.store(true);
    storage_ready_.store(driver_ != "minio");

    LOG_INFO("StorageManager initialized with driver: {} endpoint: {}", driver_, config_.endpoint);
    if (driver_ == "minio") {
        startBackgroundInitialization();
    }
    return true;
}

void StorageManager::shutdown() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (init_thread_.joinable()) {
        init_thread_.join();
    }
}

// Helper: apply S3v4 auth to a CURL handle for a given method+path
void applySigV4(CURL* curl, struct curl_slist*& headers,
                const std::string& method, const std::string& uri,
                const std::string& endpoint, const std::string& access_key,
                const std::string& secret_key,
                const std::string& payload_hash = "UNSIGNED-PAYLOAD") {
    std::string host = parseHost(endpoint);
    auto auth = signV4(method, uri, host, access_key, secret_key, payload_hash);

    headers = curl_slist_append(headers, ("Authorization: " + auth.authorization).c_str());
    headers = curl_slist_append(headers, ("x-amz-date: " + auth.amz_date).c_str());
    headers = curl_slist_append(headers, ("x-amz-content-sha256: " + auth.content_sha256).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
}

void StorageManager::startBackgroundInitialization() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (background_init_in_progress_.load()) {
        return;
    }

    if (init_thread_.joinable()) {
        init_thread_.join();
    }

    background_init_in_progress_.store(true);
    init_thread_ = std::thread([this]() {
        ensureBucketsExist();
        background_init_in_progress_.store(false);
    });
}

bool StorageManager::createBucket(const std::string& name, long timeout_ms) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_WARN("Bucket '{}' — failed to create CURL handle", name);
        return false;
    }

    std::string url = config_.endpoint + "/" + name;
    std::string uri = "/" + name + "/";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    std::vector<char> resp;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    struct curl_slist* headers = nullptr;
    applySigV4(curl, headers, "PUT", uri, config_.endpoint,
               config_.access_key, config_.secret_key);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_WARN("Bucket '{}' — CURL error: {}", name, curl_easy_strerror(res));
        return false;
    }

    if (response_code == 200 || response_code == 409) {
        LOG_INFO("Bucket '{}' ready (HTTP {})", name, response_code);
        return true;
    }

    std::string body(resp.begin(), resp.end());
    if (body.find("BucketAlreadyOwnedByYou") != std::string::npos) {
        LOG_INFO("Bucket '{}' already exists", name);
        return true;
    }

    LOG_WARN("Bucket '{}' creation returned HTTP {} — {}", name, response_code,
             body.substr(0, std::min(body.size(), static_cast<size_t>(200))));
    return false;
}

bool StorageManager::ensureBucketsExist() {
    if (!initialized_.load()) return false;
    if (driver_ != "minio") {
        storage_ready_.store(true);
        return true;
    }

    constexpr int kMaxAttempts = 2;
    constexpr long kAttemptTimeoutMs = 1000;
    constexpr auto kRetryDelay = std::chrono::milliseconds(250);

    storage_ready_.store(false);

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const bool recordings_ok = createBucket(config_.bucket_recordings, kAttemptTimeoutMs);
        const bool snapshots_ok = createBucket(config_.bucket_snapshots, kAttemptTimeoutMs);

        if (recordings_ok && snapshots_ok) {
            storage_ready_.store(true);
            LOG_INFO("StorageManager: All buckets ready");
            return true;
        }

        if (attempt < kMaxAttempts) {
            LOG_WARN("StorageManager: Bucket initialization attempt {}/{} failed; retrying once in {} ms",
                     attempt, kMaxAttempts, kRetryDelay.count());
            std::this_thread::sleep_for(kRetryDelay);
        }
    }

    storage_ready_.store(false);
    LOG_WARN("StorageManager: MinIO unavailable after {} attempts; storage will remain disabled until restart",
             kMaxAttempts);
    return false;
}

bool StorageManager::uploadFile(const std::string& local_path, const std::string& object_key) {
    if (driver_ != "minio" || !initialized_.load() || !storage_ready_.load()) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FILE* f = fopen(local_path.c_str(), "rb");
    if (!f) { curl_easy_cleanup(curl); return false; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string uri = "/" + config_.bucket_recordings + "/" + object_key;
    std::string url = config_.endpoint + uri;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, f);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);

    // Discard response
    std::vector<char> resp;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    struct curl_slist* headers = nullptr;
    applySigV4(curl, headers, "PUT", uri, config_.endpoint,
               config_.access_key, config_.secret_key);

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = 0;
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    fclose(f);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || (code != 200 && code != 204)) {
        std::string body(resp.begin(), resp.end());
        LOG_WARN("Upload '{}' failed: CURL={} HTTP {} — err: {} — body: {}", object_key, (int)res, code,
                 errbuf, body.substr(0, std::min(body.size(), (size_t)200)));
        return false;
    }
    return true;
}

// Header callback: scrape `Content-Range: bytes <start>-<end>/<total>` and
// stash the parsed total into the long long pointed to by userdata.
static size_t curlContentRangeHeaderCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    const size_t total = size * nitems;
    auto* total_size_out = static_cast<long long*>(userdata);
    constexpr const char* kPrefix = "content-range:";
    constexpr size_t kPrefixLen = 14;
    if (total > kPrefixLen) {
        // Header names are case-insensitive — compare lower.
        bool match = true;
        for (size_t i = 0; i < kPrefixLen; ++i) {
            char c = buffer[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            if (c != kPrefix[i]) { match = false; break; }
        }
        if (match) {
            std::string_view line(buffer + kPrefixLen, total - kPrefixLen);
            // Format: " bytes start-end/total\r\n"  OR  " bytes */total\r\n"
            const auto slash = line.find('/');
            if (slash != std::string_view::npos) {
                auto rest = line.substr(slash + 1);
                long long parsed = 0;
                bool ok = !rest.empty();
                for (char c : rest) {
                    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') break;
                    if (c < '0' || c > '9') { ok = false; break; }
                    parsed = parsed * 10 + (c - '0');
                }
                if (ok) *total_size_out = parsed;
            }
        }
    }
    return total;
}

StorageManager::RangeResult
StorageManager::getObjectRange(const std::string& object_key, size_t start, size_t length) {
    RangeResult out;
    if (length == 0) return out;
    if (driver_ != "minio" || !initialized_.load() || !storage_ready_.load()) return out;

    CURL* curl = curl_easy_init();
    if (!curl) return out;

    std::string uri = "/" + config_.bucket_recordings + "/" + object_key;
    std::string url = config_.endpoint + uri;
    std::string range_value = "bytes=" + std::to_string(start) + "-" +
                              std::to_string(start + length - 1);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.data);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlContentRangeHeaderCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.total_size);
    // Cap how much body we'll accept even if the server ignores Range and
    // returns 200 with the full object — protects against a misconfigured
    // backend forcing a multi-GB load into memory.
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(length + 1024 * 1024));

    struct curl_slist* headers = nullptr;
    // Range header is NOT in our SignedHeaders list — that's intentional and
    // matches the simplified signing used elsewhere in this file. MinIO does
    // not enforce strict-signed-Range, so the request is accepted and the
    // server slices the object server-side.
    headers = curl_slist_append(headers, ("Range: " + range_value).c_str());
    applySigV4(curl, headers, "GET", uri, config_.endpoint,
               config_.access_key, config_.secret_key);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        out.data.clear();
    }
    return out;
}

std::vector<char> StorageManager::getObject(const std::string& object_key) {
    std::vector<char> data;
    if (driver_ != "minio" || !initialized_.load() || !storage_ready_.load()) return data;

    CURL* curl = curl_easy_init();
    if (!curl) return data;

    std::string uri = "/" + config_.bucket_recordings + "/" + object_key;
    std::string url = config_.endpoint + uri;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

    struct curl_slist* headers = nullptr;
    applySigV4(curl, headers, "GET", uri, config_.endpoint,
               config_.access_key, config_.secret_key);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || code != 200) data.clear();
    return data;
}

bool StorageManager::exists(const std::string& object_key) {
    if (driver_ != "minio" || !initialized_.load() || !storage_ready_.load()) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string uri = "/" + config_.bucket_recordings + "/" + object_key;
    std::string url = config_.endpoint + uri;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    struct curl_slist* headers = nullptr;
    applySigV4(curl, headers, "HEAD", uri, config_.endpoint,
               config_.access_key, config_.secret_key);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && code == 200);
}

std::string StorageManager::getPresignedUrl(const std::string& object_key, int expires_in_sec) {
    // For internal use, direct URL is sufficient (MinIO is on same network)
    return config_.endpoint + "/" + config_.bucket_recordings + "/" + object_key;
}

bool StorageManager::remove(const std::string& object_key) {
    if (driver_ != "minio" || !initialized_.load() || !storage_ready_.load()) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string uri = "/" + config_.bucket_recordings + "/" + object_key;
    std::string url = config_.endpoint + uri;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    struct curl_slist* headers = nullptr;
    applySigV4(curl, headers, "DELETE", uri, config_.endpoint,
               config_.access_key, config_.secret_key);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK;
}

std::string StorageManager::signRequest(const std::string& method,
                                        const std::string& path,
                                        const std::string& query) {
    // Legacy interface — delegated to signV4 internally
    std::string host = parseHost(config_.endpoint);
    auto auth = signV4(method, path, host, config_.access_key, config_.secret_key);
    return auth.authorization;
}

} // namespace utils
} // namespace vms
