#pragma once
#include "brand_core.hpp"

namespace vms {
namespace core {
namespace brands {

class PelcoCore : public BrandCore {
public:
    bool probe(const CameraDiscovery::DiscoveryConfig& cfg) override;
    CameraDiscovery::DeviceInfo discover(const CameraDiscovery::DiscoveryConfig& cfg) override;
    std::vector<CameraDiscovery::CameraChannel> getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) override;
    std::string getBrandName() const override { return "Pelco"; }

    nlohmann::json getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) override;
};

} // namespace brands
} // namespace core
} // namespace vms
