#pragma once

#include <crow.h>
#include <string>
#include <algorithm>
#include "middleware/auth_middleware.h"
#include "utils/api_utils.h"
#include "utils/config.h"
#include "utils/security_headers.h"

namespace vms {
namespace server {

// CORS Middleware to force headers globally
//
// NOTE: Crow's auto-OPTIONS path (routing.h::find_route) short-circuits at
// handle_url() *before* the parser populates request headers, then calls
// res = response(204) which wipes everything. after_handle does run for
// auto-OPTIONS, but req.get_header_value("Origin") is empty at that point,
// so resolveCorsOrigin returns "" and applyCors is a no-op. As a result,
// cross-origin preflight responses cannot carry CORS headers from this
// middleware. The cross-origin FE-to-BE path is therefore expected to go
// through the Vite proxy (same-origin from the browser's perspective, no
// preflight needed) — see vms-frontend/.env + vite.config.js. If a future
// deployment must call the backend cross-origin, patch Crow's auto-OPTIONS
// to emit ACAO from req.headers directly.
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
            vms::api::ApiUtils::applyCors(res, vms::api::ApiUtils::resolveCorsOrigin(req));
        }

        // Ensure method/header hints only when CORS is actually enabled for
        // this response. applyCors() handles the config.enabled gate.
        if (!res.get_header_value("Access-Control-Allow-Origin").empty()) {
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With, Accept, Origin");
            res.set_header("Access-Control-Max-Age", "86400");
        }

        // Force 200 OK for OPTIONS (Preflight)
        if (req.method == crow::HTTPMethod::Options) {
            res.code = 200;
        }
    }
};

// HSTS Middleware — emits Strict-Transport-Security only in production
// mode (reverse-proxy in front + X-Forwarded-Proto: https). See
// include/utils/security_headers.h for the gate rationale + dev safety
// argument (we MUST NOT emit HSTS on a no-proxy box; it would poison
// browsers into refusing plain-HTTP localhost dev for a year).
//
// Skips WS upgrade responses for the same reason as CORSMiddleware —
// extra headers on a 101 Switching Protocols can confuse some clients.
struct HstsMiddleware {
    struct context {};

    void before_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {
        // No-op
    }

    void after_handle(crow::request& req, crow::response& res, context& /*ctx*/) {
        std::string upgrade = req.get_header_value("Upgrade");
        if (upgrade.empty()) upgrade = req.get_header_value("upgrade");
        std::string upgrade_lower = upgrade;
        std::transform(upgrade_lower.begin(), upgrade_lower.end(), upgrade_lower.begin(), ::tolower);
        if (upgrade_lower == "websocket") {
            return;
        }

        const auto& sec = vms::Config::getInstance().getSecurityConfig();
        const bool trusted_proxies_configured = !sec.trusted_proxies.empty();
        const std::string xfp = req.get_header_value("X-Forwarded-Proto");

        if (vms::utils::shouldEmitHsts(trusted_proxies_configured, xfp)) {
            // Only set if not already present — some controllers may
            // emit their own variant via ApiUtils helpers in the future.
            if (res.get_header_value("Strict-Transport-Security").empty()) {
                res.set_header("Strict-Transport-Security",
                               vms::utils::canonicalHstsValue());
            }
        }
    }
};

// Define the App type with CORS, HSTS, and Auth Middleware
using VmsApp = crow::App<CORSMiddleware, HstsMiddleware, middleware::AuthMiddleware>;

} // namespace server
} // namespace vms
