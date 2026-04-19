#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace vms {

using json = nlohmann::json;

/**
 * @brief JSON utility functions
 */
namespace JsonUtils {
    
    /**
     * @brief Parse JSON string safely
     * @param str JSON string
     * @return Parsed JSON object or nullopt if parsing fails
     */
    std::optional<json> parse(const std::string& str);
    
    /**
     * @brief Convert JSON to string with pretty printing
     * @param j JSON object
     * @param indent Number of spaces for indentation (0 = compact)
     * @return JSON string
     */
    std::string toString(const json& j, int indent = 0);
    
    /**
     * @brief Get value from JSON with default
     * @tparam T Value type
     * @param j JSON object
     * @param key Key to retrieve
     * @param default_value Default value if key doesn't exist
     * @return Value or default
     */
    template<typename T>
    T get(const json& j, const std::string& key, const T& default_value) {
        if (j.contains(key) && !j[key].is_null()) {
            try {
                return j[key].get<T>();
            } catch (...) {
                return default_value;
            }
        }
        return default_value;
    }
    
    /**
     * @brief Check if JSON contains key
     */
    bool has(const json& j, const std::string& key);
    
    /**
     * @brief Validate JSON schema (basic validation)
     * @param j JSON object to validate
     * @param required_fields List of required field names
     * @return true if valid, false otherwise
     */
    bool validate(const json& j, const std::vector<std::string>& required_fields);
    
} // namespace JsonUtils

} // namespace vms
