// ==============================================================
// File: include/api/event_engine_controller.h
// Advanced Event Engine REST endpoints (Rules, Zones, Alerts)
// ==============================================================

#pragma once

#include "server/vms_app.h"

namespace vms::api {

class EventEngineController {
public:
    static void registerRoutes(vms::server::VmsApp& app);
};

} // namespace vms::api
