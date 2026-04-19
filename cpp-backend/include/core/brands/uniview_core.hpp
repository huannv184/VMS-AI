#pragma once
#include "brand_core.hpp"

namespace vms {
namespace core {
namespace brands {

class UniviewCore : public BrandCore {
public:
    bool probe(const CameraDiscovery::DiscoveryConfig& cfg) override;
    CameraDiscovery::DeviceInfo discover(const CameraDiscovery::DiscoveryConfig& cfg) override;
    std::vector<CameraDiscovery::CameraChannel> getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) override;
    std::string getBrandName() const override { return "Uniview"; }

    nlohmann::json getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) override;
    void pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) override;
};

} // namespace brands
} // namespace core
} // namespace vms
