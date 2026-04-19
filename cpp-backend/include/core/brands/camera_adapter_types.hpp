#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <curl/curl.h>
#include "utils/logger.h"

namespace vms {

struct CameraConfig {
    std::string ip;
    std::string username;
    std::string password;
    int httpPort = 80;
    int rtspPort = 554;
    std::string mediamtx_url;  // default empty -> fallback "rtsp://localhost:8554"
    std::string ffmpeg_path;   // default empty -> fallback "ffmpeg"
};

struct DeviceInfo {
    std::string model;
    std::string serialNumber;
    std::string firmwareVersion;
};

struct CameraEvent {
    enum class Type { Unknown, Motion, LineCrossing, Intrusion, FaceDetection, TamperDetection };
    Type type = Type::Unknown;
    int channel = 1;
    bool active = false;
    std::string extra;
    std::string typeRaw;
};

struct PtzCommand {
    enum class Action { Start, Stop };
    enum class Move { None, Up, Down, Left, Right, ZoomIn, ZoomOut };
    Action action = Action::Start;
    Move move = Move::None;
    double panSpeed = 0.5;
    double tiltSpeed = 0.5;
    double zoomSpeed = 0.5;
};

/**
 * @brief Base camera adapter using libcurl for HTTP communication
 */
class BaseCameraAdapter {
public:
    explicit BaseCameraAdapter(const CameraConfig& cfg) : cfg_(cfg) {}
    virtual ~BaseCameraAdapter() = default;

    // Determine protocol based on port number
    std::string getProtocol(int port) const {
        return (port == 443 || port == 8443) ? "https" : "http";
    }

    std::string buildUrl(int port, const std::string& path) const {
        return getProtocol(port) + "://" + cfg_.ip + ":" + std::to_string(port) + path;
    }

    virtual bool connect() {
        // Try configured port first
        if (tryConnect(cfg_.httpPort)) return true;

        // Fallback: if port 80 failed, try 443 (HTTPS) and vice versa
        if (cfg_.httpPort == 80) {
            LOG_INFO("HTTP port 80 failed for {}, trying HTTPS port 443...", cfg_.ip);
            if (tryConnect(443)) {
                cfg_.httpPort = 443;  // Update config to use working port
                return true;
            }
        } else if (cfg_.httpPort == 443) {
            LOG_INFO("HTTPS port 443 failed for {}, trying HTTP port 80...", cfg_.ip);
            if (tryConnect(80)) {
                cfg_.httpPort = 80;
                return true;
            }
        }
        return false;
    }

    bool tryConnect(int port) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string url = buildUrl(port, getProbeEndpoint());
        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L); // New: Fail fast
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects (HTTP→HTTPS)

        // Disable SSL verification for self-signed certificates (common on cameras)
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        if (!cfg_.username.empty()) {
            std::string userpwd = cfg_.username + ":" + cfg_.password;
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, useDigestAuth() ? (long)CURLAUTH_DIGEST : (long)CURLAUTH_ANY);
        }

        char errorBuffer[CURL_ERROR_SIZE];
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
        errorBuffer[0] = 0;

        CURLcode code = curl_easy_perform(curl);
        long http_code = 0;
        if (code == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code < 200 || http_code >= 300) {
                LOG_WARN("[BaseCameraAdapter] tryConnect to {} returned HTTP Code: {}. Response: {}", url, http_code, response);
            } else {
                LOG_INFO("[BaseCameraAdapter] tryConnect to {} SUCCESS (HTTP {})", url, http_code);
            }
        } else {
            LOG_ERROR("[BaseCameraAdapter] tryConnect to {} failed. CURL Error: {} - {}", url, static_cast<int>(code), errorBuffer[0] ? errorBuffer : curl_easy_strerror(code));
        }

        curl_easy_cleanup(curl);
        return code == CURLE_OK && http_code >= 200 && http_code < 300;
    }

    virtual DeviceInfo getDeviceInfo() {
        DeviceInfo info;
        std::string body = rawGet(getDeviceInfoEndpoint());
        info.model = extractField(body, getModelFieldName());
        info.serialNumber = extractField(body, getSerialFieldName());
        info.firmwareVersion = extractField(body, getFirmwareFieldName());
        if (info.model.empty()) info.model = getBrandName() + " Camera";
        if (info.serialNumber.empty()) info.serialNumber = "Unknown";
        if (info.firmwareVersion.empty()) info.firmwareVersion = "Unknown";
        return info;
    }

    std::string rawGet(const std::string& path) {
        return httpRequest("GET", path, "");
    }

    std::string rawPost(const std::string& path, const std::string& body) {
        return httpRequest("POST", path, body);
    }

    std::string rawPut(const std::string& path, const std::string& body) {
        return httpRequest("PUT", path, body);
    }

    // Long-poll streaming GET — calls onChunk for each received data chunk.
    // Returns when onChunk returns false or connection drops.
    void rawStreamGet(const std::string& path, std::function<bool(const std::string&)> onChunk) {
        CURL* curl = curl_easy_init();
        if (!curl) return;

        std::string url = buildUrl(cfg_.httpPort, path);
        struct StreamCtx { std::function<bool(const std::string&)> cb; bool abort = false; };
        StreamCtx sctx{ onChunk, false };

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        if (!cfg_.username.empty()) {
            std::string userpwd = cfg_.username + ":" + cfg_.password;
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, useDigestAuth() ? (long)CURLAUTH_DIGEST : (long)CURLAUTH_ANY);
        }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t sz, size_t n, void* ud) -> size_t {
                StreamCtx* ctx = static_cast<StreamCtx*>(ud);
                if (ctx->abort) return 0;
                if (!ctx->cb(std::string(ptr, sz * n))) { ctx->abort = true; return 0; }
                return sz * n;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sctx);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }

    virtual bool ptzControl(const PtzCommand& cmd) {
        // Default: no-op
        return false;
    }

    virtual void startEventSubscription(std::function<void(const CameraEvent&)> callback, void* /*ctx*/) {
        // Default: no-op
        (void)callback;
    }

protected:
    CameraConfig cfg_;

    virtual std::string getProbeEndpoint() const = 0;
    virtual std::string getDeviceInfoEndpoint() const = 0;
    virtual bool useDigestAuth() const { return false; }
    virtual std::string getBrandName() const = 0;
    virtual std::string getModelFieldName() const { return "model"; }
    virtual std::string getSerialFieldName() const { return "serialNumber"; }
    virtual std::string getFirmwareFieldName() const { return "firmwareVersion"; }

    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }

    std::string httpRequest(const std::string& method, const std::string& path, const std::string& body) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        std::string url = buildUrl(cfg_.httpPort, path);
        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // New: Fail fast
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects

        // Disable SSL verification for self-signed certificates
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        if (!cfg_.username.empty()) {
            std::string userpwd = cfg_.username + ":" + cfg_.password;
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, useDigestAuth() ? (long)CURLAUTH_DIGEST : (long)CURLAUTH_ANY);
        }

        struct curl_slist* req_headers = nullptr;
        if (method == "POST" || method == "PUT") {
            if (method == "PUT") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            else curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
            // Hikvision ISAPI requires Content-Type: application/xml for all write ops
            req_headers = curl_slist_append(req_headers, "Content-Type: application/xml");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req_headers);
        }

        char errorBuffer[CURL_ERROR_SIZE];
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
        errorBuffer[0] = 0;

        CURLcode code = curl_easy_perform(curl);
        long http_code = 0;
        
        if (code == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code < 200 || http_code >= 300) {
                LOG_WARN("[BaseCameraAdapter] httpRequest {} to {} returned HTTP Code: {}. Response: {}", method, url, http_code, response);
            }
        } else {
            LOG_ERROR("[BaseCameraAdapter] httpRequest {} to {} failed. CURL Error: {} - {}", method, url, static_cast<int>(code), errorBuffer[0] ? errorBuffer : curl_easy_strerror(code));
        }
        
        if (req_headers) curl_slist_free_all(req_headers);
        curl_easy_cleanup(curl);

        return (code == CURLE_OK && http_code >= 200 && http_code < 300) ? response : "";
    }

    // Simple XML/text field extraction
    std::string extractField(const std::string& body, const std::string& fieldName) {
        // Try XML: <fieldName>value</fieldName>
        std::string openTag = "<" + fieldName + ">";
        std::string closeTag = "</" + fieldName + ">";
        auto pos1 = body.find(openTag);
        if (pos1 != std::string::npos) {
            pos1 += openTag.size();
            auto pos2 = body.find(closeTag, pos1);
            if (pos2 != std::string::npos) return body.substr(pos1, pos2 - pos1);
        }
        // Try key=value
        std::string key = fieldName + "=";
        auto pos = body.find(key);
        if (pos != std::string::npos) {
            pos += key.size();
            auto end = body.find_first_of("\r\n&", pos);
            return body.substr(pos, end - pos);
        }
        return "";
    }
};

} // namespace vms
