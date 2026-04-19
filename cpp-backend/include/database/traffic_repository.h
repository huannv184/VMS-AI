#pragma once

#include "models.h"
#include <vector>
#include <optional>
#include <string>
#include <ctime>

namespace vms {
namespace database {

/**
 * @brief Repository for traffic_counts table
 */
class TrafficRepository {
public:
    TrafficRepository() = default;
    ~TrafficRepository() = default;

    /**
     * @brief Insert a new traffic count record
     */
    bool insertCount(const TrafficCount& tc);

    /**
     * @brief Get counts for a camera within a time range
     * @param camera_id  Camera ID (-1 = all cameras)
     * @param from_ts    Start unix timestamp (0 = no limit)
     * @param to_ts      End unix timestamp (0 = now)
     * @param limit      Max rows returned
     */
    std::vector<TrafficCount> getCounts(int camera_id,
                                        std::time_t from_ts = 0,
                                        std::time_t to_ts = 0,
                                        int limit = 500);

    /**
     * @brief Get aggregated summary for a camera (today stats)
     */
    TrafficSummary getSummary(int camera_id);

    /**
     * @brief Delete records older than a given timestamp
     */
    bool pruneOld(std::time_t before_ts);
};

} // namespace database
} // namespace vms
