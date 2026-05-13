#pragma once

#ifdef signals
#pragma push_macro("signals")
#undef signals
#define VMS_RESTORE_QT_SIGNALS_MACRO
#endif

#include <crow.h>

#ifdef VMS_RESTORE_QT_SIGNALS_MACRO
#pragma pop_macro("signals")
#undef VMS_RESTORE_QT_SIGNALS_MACRO
#endif

#include <nlohmann/json.hpp>
#include <cctype>
#include <chrono>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

#include "utils/config.h"

// Include auth_middleware.h for the context type used in requirePermission().
// auth_middleware.h itself includes api_utils.h only in the .cpp — not in the header —
// so there is no circular dependency at the header level.
#include "middleware/auth_middleware.h"

namespace vms {
namespace api {

// ── RBAC Permission Model ─────────────────────────────────────────────────────
//
// role_id 1 = admin    → all permissions
// role_id 2 = operator → camera rw, ptz, events, recording r, roi w, device r
// role_id 3 = viewer   → camera r, events r, recording r, device r
//
// Add new permissions here; add them to ROLE_PERMISSIONS below.
// Middleware: call requirePermission() at the top of any mutating route handler.

enum class Permission : int {
    CAMERA_READ = 1,
    CAMERA_WRITE,
    PTZ_CONTROL,
    EVENT_READ,
    EVENT_DELETE,
    RECORDING_READ,
    RECORDING_DELETE,
    ROI_READ,
    ROI_WRITE,
    FACE_READ,
    FACE_WRITE,
    REID_READ,
    REID_WRITE,
    ALERT_READ,
    ALERT_WRITE,
    VIDEOWALL_READ,
    VIDEOWALL_WRITE,
    SITE_READ,
    SITE_WRITE,
    DEVICE_READ,
    DEVICE_WRITE,
    ANALYTICS_READ,
    SYSTEM_ADMIN,   // user management, settings, audit logs
};

inline const std::unordered_map<int, std::unordered_set<Permission>>& rolePermissions() {
    static const std::unordered_map<int, std::unordered_set<Permission>> table = {
        // Admin: all permissions
        {1, {
            Permission::CAMERA_READ, Permission::CAMERA_WRITE,
            Permission::PTZ_CONTROL,
            Permission::EVENT_READ, Permission::EVENT_DELETE,
            Permission::RECORDING_READ, Permission::RECORDING_DELETE,
            Permission::ROI_READ, Permission::ROI_WRITE,
            Permission::FACE_READ, Permission::FACE_WRITE,
            Permission::REID_READ, Permission::REID_WRITE,
            Permission::ALERT_READ, Permission::ALERT_WRITE,
            Permission::VIDEOWALL_READ, Permission::VIDEOWALL_WRITE,
            Permission::SITE_READ, Permission::SITE_WRITE,
            Permission::DEVICE_READ, Permission::DEVICE_WRITE,
            Permission::ANALYTICS_READ,
            Permission::SYSTEM_ADMIN,
        }},
        // Operator: day-to-day operations, no user management
        {2, {
            Permission::CAMERA_READ, Permission::CAMERA_WRITE,
            Permission::PTZ_CONTROL,
            Permission::EVENT_READ, Permission::EVENT_DELETE,
            Permission::RECORDING_READ, Permission::RECORDING_DELETE,
            Permission::ROI_READ, Permission::ROI_WRITE,
            Permission::FACE_READ,
            Permission::REID_READ,
            Permission::ALERT_READ,
            Permission::VIDEOWALL_READ, Permission::VIDEOWALL_WRITE,
            Permission::SITE_READ,
            Permission::DEVICE_READ,
            Permission::ANALYTICS_READ,
        }},
        // Viewer: read-only
        {3, {
            Permission::CAMERA_READ,
            Permission::EVENT_READ,
            Permission::RECORDING_READ,
            Permission::ROI_READ,
            Permission::FACE_READ,
            Permission::REID_READ,
            Permission::ALERT_READ,
            Permission::VIDEOWALL_READ,
            Permission::SITE_READ,
            Permission::DEVICE_READ,
            Permission::ANALYTICS_READ,
        }},
    };
    return table;
}

class ApiUtils {
public:
    // ── Meta helpers ──────────────────────────────────────────────
    static std::string generateRequestId() {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 15);
        const char* hex = "0123456789abcdef";
        char buf[37];
        for (int i = 0; i < 36; i++) buf[i] = hex[dist(rng)];
        buf[8] = '-'; buf[13] = '-'; buf[14] = '4';
        buf[18] = '-'; buf[19] = hex[8 + (dist(rng) % 4)];
        buf[23] = '-'; buf[36] = '\0';
        return std::string(buf);
    }

    static long long nowMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static nlohmann::json makeMeta() {
        return {
            {"request_id", generateRequestId()},
            {"timestamp", nowMillis()}
        };
    }

    // ── CORS ──────────────────────────────────────────────────────
    static std::string resolveCorsOrigin(const crow::request& req) {
        std::string origin = req.get_header_value("Origin");
        if (origin.empty()) origin = req.get_header_value("origin");
        if (origin.empty()) return "";

        const auto& cors = vms::Config::getInstance().getCorsConfig();
        if (!cors.enabled) return "";

        for (const auto& allowed : cors.origins) {
            if (allowed == "*") return origin;
        }
        for (const auto& allowed : cors.origins) {
            if (allowed == origin) return origin;
        }
        return "";
    }

    // ── Apply CORS headers to a response ──────────────────────────
    static void applyCors(crow::response& res, const std::string& origin) {
        std::string allowed_origin = origin;
        bool use_credentials = true;

        if (allowed_origin.empty() || allowed_origin == "*") {
             allowed_origin = "*";
             use_credentials = false;
        }

        res.set_header("Access-Control-Allow-Origin", allowed_origin);
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With, Accept, Origin, Range");
        res.set_header("Access-Control-Expose-Headers", "Content-Length, Content-Range, Accept-Ranges");

        if (use_credentials) {
            res.set_header("Access-Control-Allow-Credentials", "true");
        }

        res.set_header("Access-Control-Max-Age", "86400");
    }

    // ── Contract-enforced JSON response ───────────────────────────
    // Envelope: { success, data, error, meta }
    static crow::response createResponse(const nlohmann::json& body, int code = 200, const std::string& origin = "*") {
        crow::response res;
        res.code = code;
        res.set_header("Content-Type", "application/json");
        applyCors(res, origin);

        if (code != 204) {
            const bool ok = (code >= 200 && code < 300);
            nlohmann::json data_payload;

            // If caller already built the full envelope (has success+data+error)
            if (body.is_object() && body.contains("success") && body.contains("data") && body.contains("error")) {
                nlohmann::json envelope = body;
                if (!envelope.contains("meta")) envelope["meta"] = makeMeta();
                res.body = envelope.dump();
                return res;
            }

            // Otherwise, wrap the body as the data payload
            data_payload = body;

            nlohmann::json envelope = {
                {"success", ok},
                {"data",    ok ? data_payload : nlohmann::json(nullptr)},
                {"error",   nullptr},
                {"meta",    makeMeta()}
            };
            res.body = envelope.dump();
        }
        return res;
    }

    // ── Safe error wrapper for catch-blocks ───────────────────────
    // Use this when you have a `std::exception&` from an internal failure
    // (DB query, libcurl, image processing, etc.). It logs the full e.what()
    // server-side and returns a *generic* message to the client.
    //
    // Why: pre-fix the codebase had ~100 sites doing
    //   return ApiUtils::createErrorResponse(e.what(), 500, origin);
    // which leaked SQL error text (exposing column / table names),
    // libcurl messages (revealing internal upstream URLs and API keys
    // when those failures stringified the URL), and stack-y phrases
    // from third-party libs. None of those are useful to a legitimate
    // API client; all of them are useful to an attacker enumerating
    // backend internals. Audit ref: 2026-05-10 P0 #2.
    //
    // For 4xx errors that ARE user-actionable (bad input, validation),
    // continue using createErrorResponse with a hand-crafted user-safe
    // message — this helper is for 5xx-class catch-alls.
    //
    // Usage:
    //   } catch (const std::exception& e) {
    //       return ApiUtils::createSafeError(e, 500, origin, "list-events");
    //   }
    //
    // The `context` tag goes only into the server log so operators can
    // grep for the failure site without exposing it to the client.
    static crow::response createSafeError(const std::exception& e,
                                          int code = 500,
                                          const std::string& origin = "*",
                                          const std::string& context = "") {
        // Best-effort log of the real exception. spdlog's macro tolerates
        // brace-style formatting and falls through gracefully if logger is
        // not initialised yet.
        try {
            // Late include to avoid pulling spdlog into every controller TU.
            // (api_utils.h is included from most controllers; LOG_ERROR comes
            // through utils/logger.h which most of them already include —
            // we don't add the include here to keep this header lightweight.)
        } catch (...) {}

        // Generic client-facing messages. Code 5xx → never echo e.what();
        // 4xx with this helper is rare but supported.
        std::string client_msg;
        if (code >= 500) {
            client_msg = "Internal server error";
        } else if (code == 503) {
            client_msg = "Service temporarily unavailable";
        } else if (code == 408) {
            client_msg = "Request timed out";
        } else {
            // 4xx fallback — caller probably should have built a specific
            // message instead, but at least don't leak e.what().
            client_msg = "Request could not be completed";
        }
        // Server-side log line (callers can also log themselves with more
        // context). We swallow any exception from the logger to never
        // recurse. Using fprintf to stderr because spdlog isn't pulled in
        // by this header — controllers that include logger.h get richer
        // logging via LOG_ERROR before they call this helper.
        std::fprintf(stderr,
                     "[ApiUtils::createSafeError] code=%d ctx=%s exception=%s\n",
                     code,
                     context.empty() ? "-" : context.c_str(),
                     e.what());
        std::fflush(stderr);
        return createErrorResponse(client_msg, code, origin);
    }

    // ── Contract-enforced error response ──────────────────────────
    // error: { code: string, message: string }
    static crow::response createErrorResponse(const std::string& message, int code = 500, const std::string& origin = "*") {
        crow::response res;
        res.code = code;
        res.set_header("Content-Type", "application/json");
        applyCors(res, origin);

        std::string error_code;
        if (code == 400) error_code = "BAD_REQUEST";
        else if (code == 401) error_code = "UNAUTHORIZED";
        else if (code == 403) error_code = "FORBIDDEN";
        else if (code == 404) error_code = "NOT_FOUND";
        else if (code == 405) error_code = "METHOD_NOT_ALLOWED";
        else if (code == 409) error_code = "CONFLICT";
        else if (code == 413) error_code = "PAYLOAD_TOO_LARGE";
        else if (code == 501) error_code = "NOT_IMPLEMENTED";
        else if (code == 503) error_code = "SERVICE_UNAVAILABLE";
        else error_code = "INTERNAL_ERROR";

        nlohmann::json envelope = {
            {"success", false},
            {"data",    nullptr},
            {"error",   {{"code", error_code}, {"message", message}}},
            {"meta",    makeMeta()}
        };
        res.body = envelope.dump();
        return res;
    }
    
    // ── RBAC: requirePermission ───────────────────────────────────────
    // Call at the top of any route handler that needs authorization.
    // Returns a ready-to-end 401/403 response if the check fails, nullopt on success.
    //
    // Usage:
    //   if (auto err = ApiUtils::requirePermission(ctx, Permission::PTZ_CONTROL, origin)) return *err;
    static std::optional<crow::response> requirePermission(
            const vms::middleware::AuthMiddleware::context& ctx,
            Permission perm,
            const std::string& origin) {
        if (!ctx.user.has_value()) {
            return createErrorResponse("Authentication required", 401, origin);
        }
        const auto& table = rolePermissions();
        auto it = table.find(ctx.user->role_id);
        if (it == table.end() || it->second.find(perm) == it->second.end()) {
            return createErrorResponse("Forbidden: insufficient permissions", 403, origin);
        }
        return std::nullopt;
    }

    // Convenience overload for admin-only routes (role_id == 1 check).
    static std::optional<crow::response> requireAdmin(
            const vms::middleware::AuthMiddleware::context& ctx,
            const std::string& origin) {
        return requirePermission(ctx, Permission::SYSTEM_ADMIN, origin);
    }

    static std::string base64_encode(const unsigned char* data, size_t len) {
        static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        if (!data || len == 0) return "";
        
        std::string ret;
        ret.reserve(((len + 2) / 3) * 4);
        
        for (size_t i = 0; i < len; i += 3) {
            uint32_t val = (data[i] << 16) + 
                          ((i + 1 < len ? data[i + 1] : 0) << 8) + 
                          (i + 2 < len ? data[i + 2] : 0);
            
            ret.push_back(base64_chars[(val >> 18) & 0x3F]);
            ret.push_back(base64_chars[(val >> 12) & 0x3F]);
            
            if (i + 1 < len) ret.push_back(base64_chars[(val >> 6) & 0x3F]);
            else ret.push_back('=');
            
            if (i + 2 < len) ret.push_back(base64_chars[val & 0x3F]);
            else ret.push_back('=');
        }
        
        return ret;
    }

    static std::string base64_decode(const std::string &in) {
        std::string out;
        std::vector<int> T(256, -1);
        for (int i = 0; i < 64; i++) T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;

        int val = 0, valb = -8;
        for (size_t i = 0; i < in.length(); i++) {
            unsigned char c = in[i];
            if (std::isspace(c)) continue;
            if (c == '=') break; // End of base64 data
            if (T[c] == -1) throw std::invalid_argument("Invalid character in base64 string: " + std::string(1, c));
            val = (val << 6) + T[c];
            valb += 6;
            if (valb >= 0) {
                out.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }
};

} // namespace api
} // namespace vms
