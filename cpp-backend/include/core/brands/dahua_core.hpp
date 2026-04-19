#pragma once
#include "brand_core.hpp"
#include <sstream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

namespace vms {
namespace core {
namespace brands {

class DahuaCore : public BrandCore {
public:
    bool probe(const CameraDiscovery::DiscoveryConfig& cfg) override;
    CameraDiscovery::DeviceInfo discover(const CameraDiscovery::DiscoveryConfig& cfg) override;
    std::vector<CameraDiscovery::CameraChannel> getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) override;
    std::string getBrandName() const override { return "Dahua"; }

    nlohmann::json getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) override;

    bool ptzControl(const CameraDiscovery::DiscoveryConfig& cfg, const std::string& action, double speed) override;
    void pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) override;

private:
    std::string extractCgi(const std::string& body, const std::string& key);
    std::vector<std::string> splitLines(const std::string& s);
};

} // namespace brands
} // namespace core
} // namespace vms
