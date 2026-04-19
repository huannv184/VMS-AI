#pragma once

#include "server/vms_app.h"

namespace vms {
namespace api {

/**
 * @brief REST controller for traffic counting endpoints
 *
 * Routes:
 *   GET  /api/traffic/counts          - Query counts (params: camera_id, from, to, limit)
 *   POST /api/traffic/counts          - ingest a new count record (from AI worker or manual)
 *   GET  /api/traffic/summary         - Per-camera aggregated summary (today)
 */
class TrafficController {
public:
    static void registerRoutes(vms::server::VmsApp& app);
};

} // namespace api
} // namespace vms
