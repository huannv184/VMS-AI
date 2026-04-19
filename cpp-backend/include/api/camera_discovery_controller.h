#pragma once

#include "server/vms_app.h"

namespace vms {
namespace api {

/**
 * @class CameraDiscoveryController
 * @brief Handles REST API requests to auto-discover ONVIF/RTSP cameras on the local network.
 */
class CameraDiscoveryController {
public:
    static void registerRoutes(vms::server::VmsApp& app);
};

} // namespace api
} // namespace vms
