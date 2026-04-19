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

#include "core/user.h"
#include <optional>
#include <string>

namespace vms {
namespace middleware {

class AuthMiddleware {
public:
    struct context {
        std::optional<vms::core::User> user;
    };

    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);
    
    // Manual validation helper
    bool validate(const crow::request& req);
    bool validateToken(const std::string& token, vms::core::User& user_out);
};

} // namespace middleware
} // namespace vms
