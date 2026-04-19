#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <nlohmann/json.hpp>
#include "../camera_types.h"

namespace vms {
namespace core {
namespace brands {

/**
 * @brief Base class for brand-specific core implementations
 */
class BrandCore {
public:
    virtual ~BrandCore() = default;

    /**
     * @brief Probe if the device at given configuration belongs to this brand
     */
    virtual bool probe(const CameraDiscovery::DiscoveryConfig& cfg) = 0;

    /**
     * @brief Perform detailed discovery for this brand
     */
    virtual CameraDiscovery::DeviceInfo discover(const CameraDiscovery::DiscoveryConfig& cfg) = 0;

    /**
     * @brief Get specialized configurations (e.g. fire/thermal settings)
     */
    virtual nlohmann::json getSpecializedConfig(const CameraDiscovery::DiscoveryConfig& cfg) {
        return nlohmann::json::object();
    }

    /**
     * @brief Optimized stream detection for this brand
     */
    virtual std::vector<CameraDiscovery::CameraChannel> getOptimizedStreams(const CameraDiscovery::DiscoveryConfig& cfg) = 0;

    /**
     * @brief Get brand name
     */
    virtual std::string getBrandName() const = 0;

    /**
     * @brief PTZ Control
     * action: "up", "down", "left", "right", "zoom_in", "zoom_out", "stop"
     */
    virtual bool ptzControl(const CameraDiscovery::DiscoveryConfig& cfg, const std::string& action, double speed) {
        return false;
    }

    /**
     * @brief Pull events (long polling endpoint for alerts)
     * Used by the CameraEventService to listen to ISAPI/CGI Alarms
     */
    virtual void pullEvents(const CameraDiscovery::DiscoveryConfig& cfg, std::function<bool(const std::string&)> onEvent) {
        // Default: no-op, meaning brand does not support active event pulling
    }
};

} // namespace brands
} // namespace core
} // namespace vms
