// ==============================================================
// File: src/api/event_engine_controller.cpp
// Advanced Event Engine REST endpoints implementation
// ==============================================================

#include "api/event_engine_controller.h"
#include "events/rule_engine.h"
#include "events/zone_manager.h"
#include "events/alert_router.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace vms::api {

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
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
            auto body = json::parse(req.body);
            auto rule = events::CompositeRule::fromJSON(body);
            
            if (events::RuleEngine::getInstance().addRule(rule)) {
                return ApiUtils::createResponse({{"rule_id", rule.rule_id}}, 201, origin);
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
    ([](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& engine = events::RuleEngine::getInstance();
        
        if (req.method == crow::HTTPMethod::Get) {
            auto rule = engine.getRule(id);
            if (rule) return ApiUtils::createResponse(rule->toJSON(), 200, origin);
            return ApiUtils::createErrorResponse("Rule not found", 404, origin);
        }
        
        if (req.method == crow::HTTPMethod::Put) {
            try {
                auto rule = engine.getRule(id);
                if (!rule) return ApiUtils::createErrorResponse("Rule not found", 404, origin);
                
                auto body = json::parse(req.body);
                auto updatedRule = events::CompositeRule::fromJSON(body);
                updatedRule.rule_id = id; // Ensure ID cannot be changed
                
                if (engine.updateRule(updatedRule)) {
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
            if (engine.removeRule(id)) return ApiUtils::createResponse(json::object(), 200, origin);
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
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        try {
            auto body = json::parse(req.body);
            auto zone = events::Zone::fromJSON(body);
            
            // Auto increment ID if not provided
            if (zone.zone_id <= 0) {
                 zone.zone_id = std::time(nullptr) % 100000;
            }
            
            if (events::ZoneManager::getInstance().addZone(zone)) {
                return ApiUtils::createResponse({{"zone_id", zone.zone_id}}, 201, origin);
            }
            return ApiUtils::createErrorResponse("Failed to create zone", 500, origin);
        } catch (const json::exception& e) {
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // ZONE CRUD (GET, PUT, DELETE) by ID
    CROW_ROUTE(app, "/api/zones/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        
        auto& mgr = events::ZoneManager::getInstance();
        
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
                auto updatedZone = events::Zone::fromJSON(body);
                updatedZone.zone_id = id;
                
                if (mgr.updateZone(updatedZone)) {
                    return ApiUtils::createResponse(json::object(), 200, origin);
                }
                return ApiUtils::createErrorResponse("Failed to update zone", 500, origin);
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
