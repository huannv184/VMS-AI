#pragma once

#include "server/vms_app.h"

namespace vms {
namespace api {

class ReIDController {
public:
    static void registerRoutes(vms::server::VmsApp& app);
};

} // namespace api
} // namespace vms
