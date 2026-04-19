#include "utils/json_utils.h"

namespace vms {
namespace JsonUtils {

std::optional<json> parse(const std::string& str) {
    try {
        return json::parse(str);
    } catch (const json::parse_error& e) {
        // Return nullopt on parse error
        return std::nullopt;
    }
}

std::string toString(const json& j, int indent) {
    return j.dump(indent);
}

bool has(const json& j, const std::string& key) {
    return j.contains(key) && !j[key].is_null();
}

bool validate(const json& j, const std::vector<std::string>& required_fields) {
    for (const auto& field : required_fields) {
        if (!has(j, field)) {
            return false;
        }
    }
    return true;
}

} // namespace JsonUtils
} // namespace vms
