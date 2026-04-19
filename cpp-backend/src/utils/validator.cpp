#include "utils/validator.h"
#include <regex>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace vms {
namespace Validator {

namespace {

std::string trimCopy(const std::string& input) {
    auto begin = std::find_if_not(input.begin(), input.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();
    if (begin >= end) return "";
    return std::string(begin, end);
}

} // namespace

bool isValidCameraId(int camera_id) {
    return camera_id >= 0 && camera_id < 100;
}

bool isValidRtspUrl(const std::string& url) {
    // Accept webcam:// local device URLs (e.g. webcam://0, webcam://USB Camera)
    if (url.rfind("webcam://", 0) == 0) return true;

    std::regex rtsp_regex(
        R"(^rtsp://(?:[a-zA-Z0-9_.~%!$&'()*+,;=:-]+(?::[a-zA-Z0-9_.~%!$&'()*+,;=:-]*)?@)?[a-zA-Z0-9\.-]+(?::\d+)?(?:/.*)?$)",
        std::regex::icase
    );
    return std::regex_match(url, rtsp_regex);
}

bool isValidIpAddress(const std::string& value) {
    std::regex ipv4_regex(
        R"(^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$)"
    );
    return std::regex_match(value, ipv4_regex);
}

bool isValidHostname(const std::string& value) {
    if (value.empty() || value.size() > 253) return false;
    std::regex hostname_regex(
        R"(^(?=.{1,253}$)(?:(?!-)[A-Za-z0-9-]{1,63}(?<!-)\.)*(?:(?!-)[A-Za-z0-9-]{1,63}(?<!-))$)"
    );
    return std::regex_match(value, hostname_regex);
}

bool isValidPort(int port) {
    return port > 0 && port <= 65535;
}

bool isSafeCredential(const std::string& value, size_t max_length) {
    if (value.size() > max_length) return false;
    for (unsigned char ch : value) {
        if (std::iscntrl(ch)) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> normalizeHost(const std::string& value) {
    std::string trimmed = trimCopy(value);
    if (trimmed.empty()) return std::nullopt;
    if (!isValidIpAddress(trimmed) && !isValidHostname(trimmed)) {
        return std::nullopt;
    }
    return trimmed;
}

std::optional<std::string> normalizeRtspUrl(const std::string& raw_url) {
    std::string trimmed = trimCopy(raw_url);
    if (trimmed.empty() || !isValidRtspUrl(trimmed)) {
        return std::nullopt;
    }
    return trimmed;
}

bool isValidEmail(const std::string& email) {
    std::regex email_regex(
        R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)"
    );
    return std::regex_match(email, email_regex);
}

bool isValidROIPoints(const std::vector<std::vector<float>>& points, size_t min_points) {
    if (points.size() < min_points) return false;
    for (const auto& point : points) {
        if (point.size() != 2) return false;
        float x = point[0]; float y = point[1];
        if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return false;
        if (std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y)) return false;
    }
    return true;
}

bool isValidString(const std::string& str, size_t max_length) {
    const std::string trimmed = trimCopy(str);
    if (trimmed.empty()) return false;
    if (trimmed.length() > max_length) return false;
    
    // Support UTF-8 (Vietnamese): Allow characters >= 32
    for (unsigned char c : trimmed) {
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') {
            return false;
        }
    }
    return true;
}

std::string sanitize(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    for (unsigned char c : str) {
        if (c >= 127 || std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '@' || c == '/') {
            result += c;
        }
    }
    return result;
}

bool isInRange(int value, int min, int max) {
    return value >= min && value <= max;
}

} // namespace Validator
} // namespace vms
