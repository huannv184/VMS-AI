#pragma once

#include "server/vms_app.h"
#include "database/traffic_repository.h"
#include "database/event_repository.h"

namespace vms {
namespace api {

/**
 * @brief Controller for analytics reporting and data export
 */
class ReportingController {
public:
    ReportingController() = default;
    ~ReportingController() = default;

    /**
     * @brief Register all reporting routes
     */
    static void registerRoutes(vms::server::VmsApp& app);

private:
    /**
     * @brief Formats traffic data as CSV
     */
    static std::string formatTrafficToCsv(const std::vector<vms::TrafficCount>& data);

    /**
     * @brief Formats event data as CSV
     */
    static std::string formatEventsToCsv(const std::vector<vms::Event>& data);
};

} // namespace api
} // namespace vms
