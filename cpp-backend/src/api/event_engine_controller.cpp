// ==============================================================
// File: src/api/event_engine_controller.cpp
// Advanced Event Engine REST endpoints implementation
// ==============================================================

#include "api/event_engine_controller.h"
#include "events/rule_engine.h"
#include "events/zone_manager.h"
#include "events/alert_router.h"
#include "core/roi_manager.h"
#include "middleware/auth_middleware.h"
#include "server/vms_app.h"
#include "utils/api_utils.h"
#include "utils/config.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <database/models.h>

using json = nlohmann::json;

namespace vms::api {

namespace {

// BUG-23: rule/zone mutate routes used to skip RBAC entirely — any logged-in
// user (even a viewer) could create, update, or delete rules. Centralised
// helper so the same check is applied uniformly across all mutate paths.
std::optional<crow::response> requireRuleAdmin(
        vms::server::VmsApp& app,
        const crow::request& req,
        const std::string& origin) {
    if (!vms::Config::getInstance().getAuthConfig().enabled) {
        return std::nullopt; // dev mode — auth disabled globally
    }
    auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
    return ApiUtils::requireAdmin(ctx, origin);
}

// AiRuleEditor sends { type, points:[{x,y}], camera_id, name } using a 0-100%
// scale, while Zone::fromJSON reads "polygon" + "camera_ids" and ROIManager
// expects 0-1 normalized. Apply the bridge to both create and update so
// editing a zone keeps it in sync — previously update parsed an empty polygon
// and dropped the ROI binding entirely.
void bridgeZoneBody(json& body) {
    if (!body.contains("polygon") && body.contains("points")) {
        body["polygon"] = body["points"];
    }
    if (!body.contains("camera_ids") && body.contains("camera_id")) {
        body["camera_ids"] = json::array({ body["camera_id"].get<int>() });
    }
}

// Sync the drawn polygon with ROIManager so AiEventProcessor filters live
// detections against the latest geometry. Lookup-by-(camera_id, name) is
// best-effort — ZoneManager doesn't persist the bridge ROI id today, so on
// rename we may leave a stale ROI behind. That's acceptable for the current
// flow; a proper fix would store roi_id on the Zone row.
void syncZoneROI(const json& body, const std::string& zone_name) {
    if (!body.contains("camera_id") || !body.contains("polygon")) return;
    try {
        const int camera_id = body.value("camera_id", 0);
        const std::string name = zone_name.empty() ? body.value("name", "Zone ROI") : zone_name;
        const std::string roi_type = body.value("type", "polygon") == "line" ? "line" : "polygon";

        json pts = json::array();
        for (const auto& pt : body["polygon"]) {
            float x = pt.value("x", 0.0f) / 100.0f;
            float y = pt.value("y", 0.0f) / 100.0f;
            pts.push_back({{"x", x}, {"y", y}});
        }
        const std::string points_json = pts.dump();

        auto& roi_mgr = vms::core::ROIManager::getInstance();
        auto existing = roi_mgr.getCameraROIs(camera_id);
        for (auto& r : existing) {
            if (r.name == name) {
                r.roi_type     = roi_type;
                r.points_json  = points_json;
                r.enabled      = true;
                roi_mgr.updateROI(r);
                return;
            }
        }
        ROI roi;
        roi.camera_id   = camera_id;
        roi.name        = name;
        roi.roi_type    = roi_type;
        roi.enabled     = true;
        roi.created_at  = std::time(nullptr);
        roi.points_json = points_json;
        roi_mgr.addROI(roi);
    } catch (...) {}
}

} // namespace

// PENDING-AUDIT-2026-05-09: 4 empty-capture handlers (rules, zones, alerts/triggers, rules/stats).
// Tracked in past-bugs.md → BUG-LINT-CONTROLLERS-PENDING-2026-05-09.
// LINT-ALLOW-NO-AUTH: PENDING-AUDIT-2026-05-09 (rules CRUD)
// LINT-ALLOW-NO-AUTH: PENDING-AUDIT-2026-05-09 (zones CRUD)
// LINT-ALLOW-NO-AUTH: PENDING-AUDIT-2026-05-09 (alerts triggers GET)
// LINT-ALLOW-NO-AUTH: PENDING-AUDIT-2026-05-09 (rules stats GET)
void EventEngineController::registerRoutes(vms::server::VmsApp& app) {

    // ============================================================================
    // RULES API
    // ============================================================================
    
    // GET all rules
    CROW_ROUTE(app, "/api/rules")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
            auto rules = events::RuleEngine::getInstance().getAllRules();
            json rules_arr = json::array();
            for (const auto& r : rules) {
                rules_arr.push_back(r.toJSON());
            }
            return ApiUtils::createResponse({{"rules", rules_arr}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });
    
    // POST create rule
    CROW_ROUTE(app, "/api/rules/create")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        if (auto err = requireRuleAdmin(app, req, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            auto rule = events::CompositeRule::fromJSON(body);

            // BUG-H5 FIX: addRule now returns the assigned id. Previously the
            // controller returned `rule.rule_id` of the local input copy, which
            // was always 0 when the client omitted rule_id (the engine assigned
            // a real id only to its internal copy).
            int assigned = events::RuleEngine::getInstance().addRule(rule);
            if (assigned > 0) {
                // Persist immediately. Without this, the rule lives only in
                // memory and is lost on restart — and saveToDatabase()'s
                // DELETE WHERE id NOT IN (...) cleanup means a later save
                // wipes it from DB even if a separate path called save first.
                if (!events::RuleEngine::getInstance().saveToDatabase()) {
                    LOG_ERROR("[EventEngine] Rule {} created in memory but DB persist failed", assigned);
                    return ApiUtils::createErrorResponse("Rule created but failed to persist", 500, origin);
                }
                return ApiUtils::createResponse({{"rule_id", assigned}}, 201, origin);
            }
            return ApiUtils::createErrorResponse("Failed to create rule", 500, origin);
        } catch (const json::exception& e) {
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // RULE CRUD (GET, PUT, DELETE) by ID
    CROW_ROUTE(app, "/api/rules/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& engine = events::RuleEngine::getInstance();

        // PUT/DELETE require admin; GET stays readable to any authenticated user.
        if (req.method != crow::HTTPMethod::Get) {
            if (auto err = requireRuleAdmin(app, req, origin)) return std::move(*err);
        }

        if (req.method == crow::HTTPMethod::Get) {
            auto rule = engine.getRule(id);
            if (rule.has_value()) return ApiUtils::createResponse(rule->toJSON(), 200, origin);
            return ApiUtils::createErrorResponse("Rule not found", 404, origin);
        }

        if (req.method == crow::HTTPMethod::Put) {
            try {
                auto rule = engine.getRule(id);
                if (!rule.has_value()) return ApiUtils::createErrorResponse("Rule not found", 404, origin);
                
                auto body = json::parse(req.body);
                auto updatedRule = events::CompositeRule::fromJSON(body);
                updatedRule.rule_id = id; // Ensure ID cannot be changed
                
                if (engine.updateRule(updatedRule)) {
                    if (!engine.saveToDatabase()) {
                        LOG_ERROR("[EventEngine] Rule {} updated in memory but DB persist failed", id);
                        return ApiUtils::createErrorResponse("Rule updated but failed to persist", 500, origin);
                    }
                    return ApiUtils::createResponse(json::object(), 200, origin);
                }
                return ApiUtils::createErrorResponse("Failed to update rule", 500, origin);
            } catch (const json::exception& e) {
                return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }
        
        if (req.method == crow::HTTPMethod::Delete) {
            if (engine.removeRule(id)) {
                if (!engine.saveToDatabase()) {
                    LOG_ERROR("[EventEngine] Rule {} removed in memory but DB persist failed", id);
                    return ApiUtils::createErrorResponse("Rule removed but failed to persist", 500, origin);
                }
                return ApiUtils::createResponse(json::object(), 200, origin);
            }
            return ApiUtils::createErrorResponse("Rule not found", 404, origin);
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // ============================================================================
    // ZONES API
    // ============================================================================
    
    // GET all zones
    CROW_ROUTE(app, "/api/zones")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
            auto zones = events::ZoneManager::getInstance().getAllZones();
            json zones_arr = json::array();
            for (const auto& z : zones) {
                zones_arr.push_back(z.toJSON());
            }
            return ApiUtils::createResponse({{"zones", zones_arr}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // POST create zone
    CROW_ROUTE(app, "/api/zones/create")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        if (auto err = requireRuleAdmin(app, req, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            bridgeZoneBody(body);

            auto zone = events::Zone::fromJSON(body);
            if (zone.zone_id <= 0) {
                // BUG-24: Was `time(nullptr) % 100000` — two zones created in
                // the same second collided, and the upsert silently overwrote
                // the earlier zone (BUG-DB-01 pattern again). Use max(existing)+1.
                int next_id = 1;
                for (const auto& existing : events::ZoneManager::getInstance().getAllZones()) {
                    if (existing.zone_id >= next_id) next_id = existing.zone_id + 1;
                }
                zone.zone_id = next_id;
            }

            if (!events::ZoneManager::getInstance().addZone(zone)) {
                return ApiUtils::createErrorResponse("Failed to create zone", 500, origin);
            }

            syncZoneROI(body, zone.name);

            return ApiUtils::createResponse({{"zone_id", zone.zone_id}}, 201, origin);
        } catch (const json::exception& e) {
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // ZONE CRUD (GET, PUT, DELETE) by ID
    CROW_ROUTE(app, "/api/zones/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& mgr = events::ZoneManager::getInstance();

        if (req.method != crow::HTTPMethod::Get) {
            if (auto err = requireRuleAdmin(app, req, origin)) return std::move(*err);
        }

        if (req.method == crow::HTTPMethod::Get) {
            auto zone = mgr.getZone(id);
            if (zone) return ApiUtils::createResponse(zone->toJSON(), 200, origin);
            return ApiUtils::createErrorResponse("Zone not found", 404, origin);
        }
        
        if (req.method == crow::HTTPMethod::Put) {
            try {
                auto zone = mgr.getZone(id);
                if (!zone) return ApiUtils::createErrorResponse("Zone not found", 404, origin);

                auto body = json::parse(req.body);
                bridgeZoneBody(body);

                auto updatedZone = events::Zone::fromJSON(body);
                updatedZone.zone_id = id;

                if (!mgr.updateZone(updatedZone)) {
                    return ApiUtils::createErrorResponse("Failed to update zone", 500, origin);
                }

                syncZoneROI(body, updatedZone.name);
                return ApiUtils::createResponse(json::object(), 200, origin);
            } catch (const json::exception& e) {
                return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }
        
        if (req.method == crow::HTTPMethod::Delete) {
            if (mgr.removeZone(id)) return ApiUtils::createResponse(json::object(), 200, origin);
            return ApiUtils::createErrorResponse("Zone not found", 404, origin);
        }
        
        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // ============================================================================
    // ALERTS & TRIGGERS API
    // ============================================================================
    
    // GET recent rule triggers
    CROW_ROUTE(app, "/api/alerts/triggers")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
            int limit = 100;
            if (req.url_params.get("limit") != nullptr) {
                limit = std::stoi(req.url_params.get("limit"));
            }
            
            auto triggers = events::RuleEngine::getInstance().getRecentTriggers(limit);
            json triggers_arr = json::array();
            for (auto rit = triggers.rbegin(); rit != triggers.rend(); ++rit) {
                triggers_arr.push_back(rit->toJSON());
            }
            return ApiUtils::createResponse({{"triggers", triggers_arr}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET rule engine statistics
    CROW_ROUTE(app, "/api/rules/stats")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
             return ApiUtils::createResponse(
                 events::RuleEngine::getInstance().getStatistics(), 200, origin);
        } catch (const std::exception& e) {
             return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });
}

} // namespace vms::api
