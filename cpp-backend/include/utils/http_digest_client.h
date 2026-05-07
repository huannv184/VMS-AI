#pragma once

#include <curl/curl.h>

#include <string>
#include <utility>
#include <vector>

#include "utils/validator.h"

namespace vms::http {

struct DigestResponse {
    long status_code{0};
    std::string body;
    std::string error;

    bool ok() const {
        return status_code >= 200 && status_code < 300;
    }
};

namespace detail {

inline size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<std::string*>(userdata);
    buffer->append(ptr, size * nmemb);
    return size * nmemb;
}

inline DigestResponse invalidResponse(const std::string& message) {
    DigestResponse response;
    response.status_code = -1;
    response.error = message;
    return response;
}

} // namespace detail

inline DigestResponse requestDigest(const std::string& host,
                                    int port,
                                    const std::string& path,
                                    const std::string& method,
                                    const std::string& username,
                                    const std::string& password,
                                    const std::string& body = "",
                                    const std::string& content_type = "application/xml",
                                    long connect_timeout_seconds = 3L,
                                    long timeout_seconds = 5L,
                                    const std::vector<std::string>& extra_headers = {}) {
    auto normalized_host = vms::Validator::normalizeHost(host);
    if (!normalized_host.has_value()) {
        return detail::invalidResponse("Invalid host");
    }
    if (!vms::Validator::isValidPort(port)) {
        return detail::invalidResponse("Invalid port");
    }
    if (!vms::Validator::isSafeCredential(username) || !vms::Validator::isSafeCredential(password, 256)) {
        return detail::invalidResponse("Invalid credentials");
    }
    if (path.empty() || path.front() != '/') {
        return detail::invalidResponse("Invalid path");
    }

    DigestResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.status_code = -1;
        response.error = "curl_easy_init failed";
        return response;
    }

    std::string url = "http://" + normalized_host.value() + ":" + std::to_string(port) + path;
    std::string response_body;
    std::string auth = username + ":" + password;
    char error_buffer[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail::writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_DIGEST));

    struct curl_slist* headers = nullptr;
    if (!content_type.empty()) {
        headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());
    }
    for (const auto& header : extra_headers) {
        headers = curl_slist_append(headers, header.c_str());
    }
    if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (method == "PUT" || method == "POST") {
        if (method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        } else {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        }
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }

    CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
        response.body = std::move(response_body);
    } else {
        response.status_code = -1;
        response.error = error_buffer[0] ? error_buffer : curl_easy_strerror(code);
    }

    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return response;
}

inline DigestResponse getDigest(const std::string& host,
                                int port,
                                const std::string& path,
                                const std::string& username,
                                const std::string& password,
                                long connect_timeout_seconds = 3L,
                                long timeout_seconds = 5L) {
    return requestDigest(host, port, path, "GET", username, password, "",
                         "", connect_timeout_seconds, timeout_seconds);
}

inline DigestResponse putDigest(const std::string& host,
                                int port,
                                const std::string& path,
                                const std::string& username,
                                const std::string& password,
                                const std::string& body = "",
                                const std::string& content_type = "application/xml",
                                long connect_timeout_seconds = 3L,
                                long timeout_seconds = 5L) {
    return requestDigest(host, port, path, "PUT", username, password, body,
                         content_type, connect_timeout_seconds, timeout_seconds);
}

inline DigestResponse deleteDigest(const std::string& host,
                                   int port,
                                   const std::string& path,
                                   const std::string& username,
                                   const std::string& password,
                                   long connect_timeout_seconds = 3L,
                                   long timeout_seconds = 5L) {
    return requestDigest(host, port, path, "DELETE", username, password, "",
                         "", connect_timeout_seconds, timeout_seconds);
}

} // namespace vms::http
