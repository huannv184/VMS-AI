#pragma once

#include <string>
#include <vector>
#include <optional>

namespace vms {

/**
 * @brief Input validation utilities
 */
namespace Validator {
    
    /**
     * @brief Validate camera ID
     * @param camera_id Camera ID to validate
     * @return true if valid (0-9), false otherwise
     */
    bool isValidCameraId(int camera_id);
    
    /**
     * @brief Validate RTSP URL format
     * @param url RTSP URL
     * @return true if valid format, false otherwise
     */
    bool isValidRtspUrl(const std::string& url);

    bool isValidIpAddress(const std::string& value);
    bool isValidHostname(const std::string& value);
    bool isValidPort(int port);
    bool isSafeCredential(const std::string& value, size_t max_length = 128);
    std::optional<std::string> normalizeHost(const std::string& value);
    std::optional<std::string> normalizeRtspUrl(const std::string& raw_url);
    
    /**
     * @brief Validate email format
     * @param email Email address
     * @return true if valid format, false otherwise
     */
    bool isValidEmail(const std::string& email);
    
    /**
     * @brief Validate ROI points (normalized coordinates)
     * @param points List of [x, y] coordinates (0.0 to 1.0)
     * @param min_points Minimum number of points required
     * @return true if valid, false otherwise
     */
    bool isValidROIPoints(const std::vector<std::vector<float>>& points, size_t min_points = 3);
    
    /**
     * @brief Validate string is not empty and within length limits
     * @param str String to validate
     * @param max_length Maximum allowed length
     * @return true if valid, false otherwise
     */
    bool isValidString(const std::string& str, size_t max_length = 255);
    
    /**
     * @brief Sanitize string (remove dangerous characters)
     * @param str Input string
     * @return Sanitized string
     */
    std::string sanitize(const std::string& str);
    
    /**
     * @brief Validate integer is within range
     * @param value Value to validate
     * @param min Minimum value (inclusive)
     * @param max Maximum value (inclusive)
     * @return true if valid, false otherwise
     */
    bool isInRange(int value, int min, int max);
    
} // namespace Validator

} // namespace vms
