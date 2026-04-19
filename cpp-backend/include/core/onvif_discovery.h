#pragma once
#include <vector>
#include <string>

namespace vms {
namespace core {

struct OnvifDevice {
    std::string ip;
    std::string service_url;
    std::string name;
    std::string manufacturer;
    std::string model;
};

class OnvifDiscovery {
public:
    static std::vector<OnvifDevice> discover(int timeout_ms = 3000);
};

} // namespace core
} // namespace vms
