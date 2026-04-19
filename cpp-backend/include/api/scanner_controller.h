#pragma once
#include <crow.h>
#include "server/vms_app.h"

namespace vms {
namespace api {

class ScannerController {
public:
    static void registerRoutes(vms::server::VmsApp& app);
};

} // namespace api
} // namespace vms
