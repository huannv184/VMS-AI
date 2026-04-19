#pragma once

#include "server/vms_app.h"
#include "database/user_repository.h"

namespace vms {
namespace api {

class UserController {
public:
    static void registerRoutes(vms::server::VmsApp& app);
};

} // namespace api
} // namespace vms
