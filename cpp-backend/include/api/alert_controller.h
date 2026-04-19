#pragma once

#include "server/vms_app.h"

namespace vms {
namespace api {

class AlertController {
public:
    static void registerRoutes(vms::server::VmsApp& app);
};

} // namespace api
} // namespace vms
