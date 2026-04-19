#pragma once
#include "server/vms_app.h"

namespace vms {
namespace api {

/**
 * @brief Controller for analytics and counting data
 */
class AnalyticsController {
public:
    static void registerRoutes(vms::server::VmsApp& app);
};

} // namespace api
} // namespace vms
