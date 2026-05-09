# cpp-backend/cmake/lint_controllers.cmake
#
# Forward-leverage lint guard against the RBAC anti-patterns we kept
# discovering in `*_controller.cpp` files between 2026-04 and 2026-05:
#
#   1. CROW_ROUTE handlers using `[]` lambda capture instead of `[&app]`,
#      which means the handler cannot read AuthMiddleware context and
#      typically lacks a `requirePermission` call. Discovered four times
#      this week alone (BUG-ANPR-AUTH-01, BUG-SNAP-AUTH-01, BUG-HLS-AUTH-01,
#      BUG-EX-RBAC-01) plus pre-existing legacy holes in event/recording/
#      tracking/etc. controllers.
#   2. The legacy `auth.validate(req)` SEC-005 anti-pattern — checks "is
#      caller authenticated?" but skips per-route permission policy. Should
#      be `requirePermission(ctx, Permission::X, origin)` instead.
#
# How exemptions work:
#   Each `[]`-capture handler MUST be paired with a same-file
#   `// LINT-ALLOW-NO-AUTH: <short reason>` comment somewhere in the file.
#   The lint counts captures vs markers per file. New captures without
#   a matching marker fail the build at configure time.
#
# Reasons we accept:
#   - "auth-flow"      — login / logout / 2fa-verify / change-password.
#   - "explicit-public" — endpoint documented as PUBLIC for a specific
#                         operational reason (e.g. streaming-config probe
#                         before login lands).
#   - "PENDING-AUDIT-<date>" — known unaudited route from a survey pass.
#                              Tracked in .claude/memory/past-bugs.md and
#                              the controller-lint memory entry.
#
# Run ad-hoc:
#   cmake -DCONTROLLERS_DIR=cpp-backend/src/api -P cpp-backend/cmake/lint_controllers.cmake

if(NOT DEFINED CONTROLLERS_DIR)
    message(FATAL_ERROR "lint_controllers.cmake: CONTROLLERS_DIR not set")
endif()

file(GLOB controller_files "${CONTROLLERS_DIR}/*_controller.cpp")
list(LENGTH controller_files n_files)
if(n_files EQUAL 0)
    message(WARNING "lint_controllers.cmake: no *_controller.cpp under ${CONTROLLERS_DIR}")
    return()
endif()

set(any_failure FALSE)
set(report_lines "")

foreach(file ${controller_files})
    get_filename_component(short ${file} NAME)
    file(READ ${file} content)

    # `auth.validate(` should be ZERO occurrences in code (comments are fine).
    # Strip block + line comments crudely to avoid false-positive on doc text.
    string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" stripped "${content}")
    string(REGEX REPLACE "//[^\n]*" "" stripped "${stripped}")
    string(REGEX MATCHALL "auth\\.validate\\(req\\)" auth_calls "${stripped}")
    list(LENGTH auth_calls n_auth)
    if(n_auth GREATER 0)
        list(APPEND report_lines
             "  ${short}: ${n_auth} `auth.validate(req)` call(s) — switch to `requirePermission(ctx, Permission::X, origin)` (see SEC-005 / BUG-EX-RBAC-01)")
        set(any_failure TRUE)
    endif()

    # Empty-capture lambdas: `([](`
    # Markers on the original content (we want to count comment markers, so use
    # the un-stripped content here).
    string(REGEX MATCHALL "\\(\\[\\]\\(" empty_captures "${content}")
    string(REGEX MATCHALL "LINT-ALLOW-NO-AUTH" allow_markers "${content}")
    list(LENGTH empty_captures n_empty)
    list(LENGTH allow_markers n_allow)
    if(n_empty GREATER n_allow)
        math(EXPR missing "${n_empty} - ${n_allow}")
        list(APPEND report_lines
             "  ${short}: ${n_empty} `[]`-capture handler(s) but only ${n_allow} `LINT-ALLOW-NO-AUTH` marker(s) — ${missing} missing")
        set(any_failure TRUE)
    endif()
endforeach()

if(any_failure)
    set(msg "Controller RBAC lint failures:\n")
    foreach(line ${report_lines})
        set(msg "${msg}${line}\n")
    endforeach()
    set(msg "${msg}\nFix template:\n")
    set(msg "${msg}  CROW_ROUTE(app, \"/api/...\")\n")
    set(msg "${msg}    .methods(...)\n")
    set(msg "${msg}    ([&app](const crow::request& req) {\n")
    set(msg "${msg}        std::string origin = ApiUtils::resolveCorsOrigin(req);\n")
    set(msg "${msg}        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);\n")
    set(msg "${msg}        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);\n")
    set(msg "${msg}        if (auto err = ApiUtils::requirePermission(ctx, Permission::X, origin)) return std::move(*err);\n")
    set(msg "${msg}        ...\n")
    set(msg "${msg}    });\n")
    set(msg "${msg}\nIntentional unauth (login/public probes/etc.): add a same-file comment\n")
    set(msg "${msg}  // LINT-ALLOW-NO-AUTH: <reason e.g. auth-flow|explicit-public|PENDING-AUDIT-YYYY-MM-DD>\n")
    message(FATAL_ERROR "${msg}")
endif()
