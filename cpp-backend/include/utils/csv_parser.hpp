#include <string>
#include <vector>
#include <sstream>
#include <nlohmann/json.hpp>

namespace vms {
namespace utils {

class CsvParser {
public:
    /**
     * @brief Simple CSV to JSON array converter
     * Expects headers: name, rtsp_url, sub_stream_url, description
     */
    static nlohmann::json parseCameras(const std::string& csv_content) {
        nlohmann::json result = nlohmann::json::array();
        std::istringstream ss(csv_content);
        std::string line;
        
        // Read header
        if (!std::getline(ss, line)) return result;
        std::vector<std::string> headers = split(line, ',');
        
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            std::vector<std::string> values = split(line, ',');
            nlohmann::json obj = nlohmann::json::object();
            
            for (size_t i = 0; i < headers.size() && i < values.size(); ++i) {
                std::string key = trim(headers[i]);
                std::string val = trim(values[i]);
                obj[key] = val;
            }
            
            // Basic validation: must have name and rtsp_url
            if (obj.contains("name") && obj.contains("rtsp_url")) {
                result.push_back(obj);
            }
        }
        
        return result;
    }

private:
    static std::string trim(const std::string& s) {
        auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        auto last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, (last - first + 1));
    }

    static std::vector<std::string> split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }
};

} // namespace utils
} // namespace vms
