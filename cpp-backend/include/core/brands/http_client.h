#pragma once
#include "core/camera_types.h"
#include <string>
#include <curl/curl.h>
#include <functional>

namespace CameraDiscovery {

struct HttpResponse {
    long        status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    explicit HttpClient(const DiscoveryConfig& cfg) : cfg_(cfg) {}

    // GET with Basic/Digest auth
    HttpResponse get(const std::string& path,
                     bool digest = false,
                     const std::string& extra_headers = "") const
    {
        return request("GET", path, "", digest, extra_headers);
    }

    // POST with Basic/Digest auth
    HttpResponse post(const std::string& path,
                      const std::string& body,
                      bool digest = false,
                      const std::string& content_type = "application/json") const
    {
        return request("POST", path, body, digest, "Content-Type: " + content_type);
    }

    // PUT with Basic/Digest auth
    HttpResponse put(const std::string& path,
                     const std::string& body,
                     bool digest = false,
                     const std::string& content_type = "application/xml") const
    {
        return request("PUT", path, body, digest, "Content-Type: " + content_type);
    }

    // Stream GET (Long Polling)
    void streamGet(const std::string& path,
                   std::function<bool(const std::string&)> onDataChunk,
                   bool digest = false,
                   const std::string& extra_headers = "") const
    {
        requestStream(path, onDataChunk, digest, extra_headers);
    }

private:
    const DiscoveryConfig& cfg_;

    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }

    HttpResponse request(const std::string& method,
                         const std::string& path,
                         const std::string& body,
                         bool digest,
                         const std::string& extra_header) const
    {
        HttpResponse resp;
        CURL* curl = curl_easy_init();
        if (!curl) { resp.status = -1; resp.body = "curl_easy_init failed"; return resp; }

        std::string url = "http://" + cfg_.host + ":" + std::to_string(cfg_.http_port) + path;
        std::string response_body;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)cfg_.timeout_sec);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

        // Authentication
        if (!cfg_.username.empty()) {
            std::string userpwd = cfg_.username + ":" + cfg_.password;
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
            if (digest) {
                curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_DIGEST);
            } else {
                curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
            }
        }

        // Custom headers
        struct curl_slist* header_list = nullptr;
        if (!extra_header.empty()) {
            header_list = curl_slist_append(header_list, extra_header.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        }

        // Method
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        } else if (method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        }

        CURLcode code = curl_easy_perform(curl);

        if (header_list) {
            curl_slist_free_all(header_list);
        }

        if (code != CURLE_OK) {
            resp.status = -1;
            resp.body = curl_easy_strerror(code);
            curl_easy_cleanup(curl);
            return resp;
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status = http_code;
        resp.body = response_body;

        curl_easy_cleanup(curl);
        return resp;
    }

    struct StreamContext {
        std::function<bool(const std::string&)> cb;
        bool abort_flag = false;
    };

    static size_t streamWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        StreamContext* ctx = static_cast<StreamContext*>(userdata);
        if (ctx->abort_flag) return 0;

        std::string chunk(ptr, size * nmemb);
        if (!ctx->cb(chunk)) {
            ctx->abort_flag = true;
            return 0;
        }
        return size * nmemb;
    }

    void requestStream(const std::string& path,
                       std::function<bool(const std::string&)> onDataChunk,
                       bool digest,
                       const std::string& extra_header) const
    {
        CURL* curl = curl_easy_init();
        if (!curl) return;

        std::string url = "http://" + cfg_.host + ":" + std::to_string(cfg_.http_port) + path;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // No global timeout for streaming
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        // Fix: Prevent infinite block if network drops packets quietly
        // If speed drops below 1 byte/sec for 15 seconds, abort connection
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);

        StreamContext ctx;
        ctx.cb = onDataChunk;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

        // Authentication
        if (!cfg_.username.empty()) {
            std::string userpwd = cfg_.username + ":" + cfg_.password;
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
            if (digest) {
                curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_DIGEST);
            } else {
                curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
            }
        }

        struct curl_slist* header_list = nullptr;
        if (!extra_header.empty()) {
            header_list = curl_slist_append(header_list, extra_header.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        }

        curl_easy_perform(curl);

        if (header_list) {
            curl_slist_free_all(header_list);
        }
        curl_easy_cleanup(curl);
    }
};

} // namespace CameraDiscovery
