#pragma once
#include "server/vms_app.h"
#include "middleware/auth_middleware.h"
#include <crow.h>

namespace vms {
namespace api {

class SynopsisController {
public:
    static void registerRoutes(vms::server::VmsApp& app, vms::middleware::AuthMiddleware& auth);
    static void shutdown();
};

} // namespace api
} // namespace vms
