#pragma once

#include <crow.h>
#include <string>
#include <algorithm>
#include "middleware/auth_middleware.h"
#include "utils/api_utils.h"

namespace vms {
namespace server {

// CORS Middleware to force headers globally
struct CORSMiddleware {
    struct context {};
    
    void before_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {
        // No-op
    }
    
    void after_handle(crow::request& req, crow::response& res, context& /*ctx*/) {
        // Skip for WebSocket upgrade requests to avoid interfering with handshake
        std::string upgrade = req.get_header_value("Upgrade");
        if (upgrade.empty()) upgrade = req.get_header_value("upgrade");
        std::string upgrade_lower = upgrade;
        std::transform(upgrade_lower.begin(), upgrade_lower.end(), upgrade_lower.begin(), ::tolower);
        if (upgrade_lower == "websocket") {
            return;
        }

        // Only add CORS headers if they are NOT already set
        // This prevents duplication since ApiUtils::createResponse also sets them
        if (res.get_header_value("Access-Control-Allow-Origin").empty()) {
            std::string origin = vms::api::ApiUtils::resolveCorsOrigin(req);
            if (origin.empty()) {
                res.set_header("Access-Control-Allow-Origin", "*");
            } else {
                res.set_header("Access-Control-Allow-Origin", origin);
                res.set_header("Access-Control-Allow-Credentials", "true");
                res.set_header("Vary", "Origin");
            }
        }

        // Always ensure these headers are present (using set_header to overwrite/ensure single value)
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With, Accept, Origin");
        res.set_header("Access-Control-Max-Age", "86400");
        
        // Force 200 OK for OPTIONS (Preflight)
        if (req.method == crow::HTTPMethod::Options) {
            res.code = 200;
        }
    }
};

// Define the App type with CORS and Auth Middleware
using VmsApp = crow::App<CORSMiddleware, middleware::AuthMiddleware>;

} // namespace server
} // namespace vms
