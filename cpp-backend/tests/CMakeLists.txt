cmake_minimum_required(VERSION 3.21)

find_package(GTest REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenCV REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

# ── auth tests ────────────────────────────────────────────────────────────────
add_executable(test_auth test_auth.cpp)
target_include_directories(test_auth PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_auth PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME AuthTests COMMAND test_auth)

# ── password-hash tests (Argon2id activation guard) ──────────────────────────
# Locks the H6 wiring in place: if CMakeLists.txt ever loses
# `unofficial::argon2::libargon2` or the VMS_HAS_ARGON2 define, this target
# either fails to link or throws std::runtime_error at runtime — both surface
# the regression before the silent SHA256 fallback ships.
find_package(unofficial-argon2 CONFIG REQUIRED)
add_executable(test_password_hash test_password_hash.cpp)
target_include_directories(test_password_hash PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_password_hash PRIVATE VMS_HAS_ARGON2)
target_link_libraries(test_password_hash PRIVATE
    GTest::gtest GTest::gtest_main
    unofficial::argon2::libargon2)
add_test(NAME PasswordHashTests COMMAND test_password_hash)

# ── readiness-state tests (liveness/readiness split policy guard) ────────────
# Pins the boolean matrix in include/core/readiness_state.h so future
# refactors can't silently change the rule "DB required, storage gated on
# config". Pure-function tests — no Crow, no globals. The Crow handler in
# system_controller.cpp builds a ReadinessSnapshot from atomics and passes
# it to computeReady() (the function this target exercises).
add_executable(test_readiness_state test_readiness_state.cpp)
target_include_directories(test_readiness_state PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_readiness_state PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME ReadinessStateTests COMMAND test_readiness_state)

# ── storage backoff tests (H7 retry-schedule guard) ──────────────────────────
# Pins the long-lived recovery loop's sleep sequence (5s → 15s → 60s →
# 300s capped) so future "tuning" can't silently turn it into a spin-loop
# or a glacial 24-hour wait. Pure-function test — links the same
# StorageManager translation unit as the backend to keep the symbol
# resolution honest (if someone deletes nextBackoffSeconds the test
# fails to link).
add_executable(test_storage_backoff
    test_storage_backoff.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/storage_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/config.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp)
target_include_directories(test_storage_backoff PRIVATE ${CMAKE_SOURCE_DIR}/include)
# storage_manager.cpp uses std::min on Windows; match the main target's
# NOMINMAX so windows.h's min/max macros don't shadow the stdlib calls.
if(MSVC)
    target_compile_definitions(test_storage_backoff PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_link_libraries(test_storage_backoff PRIVATE
    GTest::gtest GTest::gtest_main
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    OpenSSL::SSL OpenSSL::Crypto
    CURL::libcurl)
add_test(NAME StorageBackoffTests COMMAND test_storage_backoff)

# ── HSTS gate tests (H8 production-mode header policy) ──────────────────────
# Pins the shouldEmitHsts() decision matrix so HSTS is emitted ONLY when
# operators have declared a reverse proxy AND the proxy forwarded
# X-Forwarded-Proto: https. Pure-function test — guards the "you cannot
# un-emit HSTS for a year" foot-gun the same way the Argon2 tests guard
# the silent SHA256 fallback. See include/utils/security_headers.h.
add_executable(test_hsts_header test_hsts_header.cpp)
target_include_directories(test_hsts_header PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_hsts_header PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME HstsHeaderTests COMMAND test_hsts_header)

# ── CSV escape tests (report export hardening) ────────────────────────────────
# Pins escapeCsvField against the three failure modes pre-fix
# reporting_controller hit: (1) `,` / `"` / newline breaking column alignment,
# (2) `=` / `+` / `-` / `@` triggering Excel formula execution on open, and
# (3) compound cases. Header-only, no link against backend code.
add_executable(test_csv_escape test_csv_escape.cpp)
target_include_directories(test_csv_escape PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_csv_escape PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME CsvEscapeTests COMMAND test_csv_escape)

# ── metrics auth bearer-token gate tests (H9) ────────────────────────────────
# Pins the policy matrix for /api/v1/metrics: empty token → public, configured
# token → "Bearer <token>" exact match required, no scheme case variation, no
# length oracle. Header-only helper — same shape as HstsHeaderTests.
add_executable(test_metrics_auth test_metrics_auth.cpp)
target_include_directories(test_metrics_auth PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_metrics_auth PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME MetricsAuthTests COMMAND test_metrics_auth)

# ── config validation tests (SEC-MINIO + SEC-CORS guards + legacy DB-key compat) ──
# Pins the fail-fast matrix for storage.driver=minio + default credentials,
# cors.origins=["*"] + credentials:true, and the legacy flat-key WARN path.
# Links REAL config.cpp + logger.cpp so the test fails if loader logic drifts.
add_executable(test_config_validation
    test_config_validation.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/config.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp)
target_include_directories(test_config_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_config_validation PRIVATE
    GTest::gtest GTest::gtest_main
    spdlog::spdlog
    yaml-cpp::yaml-cpp)
add_test(NAME ConfigValidationTests COMMAND test_config_validation)

# ── ApiUtils safe-error tests (no exception text leak to clients) ───────────
# Regression target: pre-fix ~108 catch-blocks across the controllers used
# `createErrorResponse(e.what(), ...)` which leaked SQL column names, libcurl
# upstream URLs, and OpenCV stack-y messages to the client. createSafeError
# logs e.what() server-side and returns a generic message. This test pins
# that the generic message is the only thing the client sees.
#
# Links REAL config.cpp + logger.cpp because ApiUtils::applyCors() reads
# Config::getInstance().getCorsConfig() (response is run through CORS even
# on the error path). Config defaults from config.h are enough; we don't
# call loadFromFile in this test.
add_executable(test_api_utils_safe_error
    test_api_utils_safe_error.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/config.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp)
target_include_directories(test_api_utils_safe_error PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_api_utils_safe_error PRIVATE
    GTest::gtest GTest::gtest_main
    Crow::Crow
    Boost::system
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    yaml-cpp::yaml-cpp)
if(WIN32)
    target_link_libraries(test_api_utils_safe_error PRIVATE Ws2_32)
endif()
add_test(NAME ApiUtilsSafeErrorTests COMMAND test_api_utils_safe_error)

# ── jwt purpose isolation tests (SEC-002 regression) ──────────────────────────
# Links the REAL jwt_utils.cpp + config.cpp + logger.cpp so the test fails if
# someone refactors away the token_use claim or accepts wrong-purpose tokens.
# Config::getInstance() works without loadFromFile — defaults from config.h
# give us a non-empty secret_key, which is all jwt_utils needs.
add_executable(test_jwt_purpose
    test_jwt_purpose.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/jwt_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/config.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp)
target_include_directories(test_jwt_purpose PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_jwt_purpose PRIVATE
    GTest::gtest GTest::gtest_main
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    jwt-cpp::jwt-cpp
    OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME JwtPurposeTests COMMAND test_jwt_purpose)

# ── db_portability tests ──────────────────────────────────────────────────────
add_executable(test_db_portability test_db_portability.cpp)
target_include_directories(test_db_portability PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_db_portability PRIVATE GTest::gtest GTest::gtest_main Qt6::Core Qt6::Sql)
add_test(NAME DbPortabilityTests COMMAND test_db_portability)

# ── attendance dedup tests ────────────────────────────────────────────────────
# Inline reproduction of AttendanceTracker dedup core (no link against backend
# code). Exercises packKey collision behaviour, cooldown drop, and resolveKind
# toggle for the 'both' role. Update test_attendance_dedup.cpp whenever the
# matching logic in src/core/attendance_tracker.cpp changes.
add_executable(test_attendance_dedup test_attendance_dedup.cpp)
target_include_directories(test_attendance_dedup PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_attendance_dedup PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME AttendanceDedupTests COMMAND test_attendance_dedup)

# ── attendance shift (night-shift correctness) tests ─────────────────────────
# Inline reproduction of the helpers in src/api/attendance_controller.cpp:
# isValidHmTime, hmToMinutes, shiftDateMidnight, lateMinutesForPunch.
# Update test_attendance_shift.cpp whenever the controller helpers change.
# Targets the regression where 22:00→06:00 shifts mis-attributed
# next-morning punches (silently "on time" when actually 8h late).
add_executable(test_attendance_shift test_attendance_shift.cpp)
target_include_directories(test_attendance_shift PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_attendance_shift PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME AttendanceShiftTests COMMAND test_attendance_shift)

# ── line crossing tests ───────────────────────────────────────────────────────
# Inline reproduction of PeopleCountTracker::checkCrossings (geometry +
# side-flip + segment-intersect gate + cooldown + class filter). Update
# test_line_crossing.cpp whenever src/core/people_count_tracker.cpp changes.
add_executable(test_line_crossing test_line_crossing.cpp)
target_include_directories(test_line_crossing PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_line_crossing PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME LineCrossingTests COMMAND test_line_crossing)

# ── counter bucket aggregation tests ──────────────────────────────────────────
# Inline reproduction of the group-by aggregation in
# src/core/counter_bucket_aggregator.cpp. Verifies direction → in/out
# mapping, ts_minute floor, and skip rules for malformed metadata.
add_executable(test_counter_bucket test_counter_bucket.cpp)
target_include_directories(test_counter_bucket PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_counter_bucket PRIVATE
    GTest::gtest GTest::gtest_main
    nlohmann_json::nlohmann_json)
add_test(NAME CounterBucketTests COMMAND test_counter_bucket)

# ── counter summary/history tests ─────────────────────────────────────────────
# Inline reproduction of the bucket → summary/history aggregation inside
# the GET /api/counter/summary and GET /api/counter/history handlers in
# src/api/counter_controller.cpp. Guards the data contract CounterView
# depends on after the swap away from /api/analytics/traffic.
add_executable(test_counter_summary test_counter_summary.cpp)
target_include_directories(test_counter_summary PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_counter_summary PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME CounterSummaryTests COMMAND test_counter_summary)

# ── PPE status / readiness tests ──────────────────────────────────────────────
# Inline reproduction of the health-classification matrix in the
# GET /api/analytics/ppe/status handler (analytics_controller.cpp) plus
# the PpeComplianceAggregator atomic-bump semantics for last_tick_ms /
# last_violation_ms. Update test_ppe_status.cpp whenever either changes.
add_executable(test_ppe_status test_ppe_status.cpp)
target_include_directories(test_ppe_status PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_ppe_status PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME PpeStatusTests COMMAND test_ppe_status)

# ── attendance health / audit tests ───────────────────────────────────────────
# Inline reproduction of the health-classification matrix in the
# GET /api/attendance/health handler (attendance_controller.cpp). Guards
# the operator contract: inactive / degraded / stale / ok semantics +
# precedence (inactive > degraded > stale > ok) + the last_event_ts == 0
# guard that prevents fresh-boot from falsely showing as stale.
add_executable(test_attendance_health test_attendance_health.cpp)
target_include_directories(test_attendance_health PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_attendance_health PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME AttendanceHealthTests COMMAND test_attendance_health)

# ── counter integration tests (PR-7 pilot — first DB-backed integration) ──────
# Links REAL DbManager + CounterBucketAggregator + Config + Logger against a
# per-test temp SQLite file. Verifies the slice the inline test_counter_bucket
# can't: real-SQLite UPSERT semantics, schema portability between init and
# aggregator/summary SQL, time-window boundary, idempotency. Bypasses
# AiEventProcessor handler — handler coverage stays on the hardware runbook
# (docs/runbooks/counter_e2e_readiness.md) per the 2026-05-25 roadmap.
add_executable(test_counter_integration
    integration/test_counter_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/database/db_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/counter_bucket_aggregator.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/config.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp)
target_include_directories(test_counter_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_counter_integration PRIVATE
    GTest::gtest GTest::gtest_main
    Qt6::Core Qt6::Sql
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    yaml-cpp::yaml-cpp)
add_test(NAME CounterIntegrationTests COMMAND test_counter_integration)

# ── PPE integration tests (PR-7B — Qt-free, no DB) ────────────────────────────
# Links the REAL PpeComplianceAggregator after PR-7B's refactor extracted it
# from ai_event_processor.cpp into its own TU. No Qt / no DbManager / no
# QCoreApplication needed — the aggregator is pure atomics + std::mutex + a
# 60-slot ring per camera. Catches reproduction-vs-prod drift that the inline
# test_ppe_status.cpp would miss when prod code evolves.
add_executable(test_ppe_integration
    integration/test_ppe_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ppe_compliance_aggregator.cpp)
target_include_directories(test_ppe_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_ppe_integration PRIVATE
    GTest::gtest GTest::gtest_main
    nlohmann_json::nlohmann_json)
add_test(NAME PpeIntegrationTests COMMAND test_ppe_integration)

# ── attendance integration tests (PR-7C — DB-backed, exercises full
# AttendanceTracker → BulkWriter → attendance_events write path against
# real SQLite, then validates the /health 24h-scan SUM(CASE WHEN) read
# query). Closes the trilogy (counter/PPE/attendance) of synthetic
# integration tests per the readiness verification roadmap.
add_executable(test_attendance_integration
    integration/test_attendance_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/core/attendance_tracker.cpp
    ${CMAKE_SOURCE_DIR}/src/database/db_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/config.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp)
target_include_directories(test_attendance_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_attendance_integration PRIVATE
    GTest::gtest GTest::gtest_main
    Qt6::Core Qt6::Sql
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    yaml-cpp::yaml-cpp)
add_test(NAME AttendanceIntegrationTests COMMAND test_attendance_integration)

# ── frame bus tests (Phase A refactor) ────────────────────────────────────────
# Links the REAL FrameBus implementation (not an inline reproduction) to catch
# regressions in subscribe/publish/unsubscribe semantics, the zero-copy raw_frame
# fan-out invariant, and concurrent publish + subscribe behaviour.
# Logger::get() returns nullptr until init() is called → LOG_INFO macros are
# no-ops here, so we don't need the full Logger.cpp at test time.
add_executable(test_frame_bus
    test_frame_bus.cpp
    ${CMAKE_SOURCE_DIR}/src/core/frame_bus.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp)
target_include_directories(test_frame_bus PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/ai/inference/include)
target_link_libraries(test_frame_bus PRIVATE
    GTest::gtest GTest::gtest_main
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    ${OpenCV_LIBS})
add_test(NAME FrameBusTests COMMAND test_frame_bus)

# ── HTTP startup fail-fast tests (BUG-HTTP-01 regression) ────────────────────
# First user of the integration/ subtree: spins real `crow::App` instances
# via the helper in `integration/crow_test_helpers.h` and asserts that
# `run_async` + `wait_for_server_start` propagate startup failures the way
# `cpp-backend/src/main.cpp` relies on.  No backend sources linked — Crow's
# primitives are what we're regressing against.
add_executable(test_http_startup_failfast
    integration/test_http_startup_failfast.cpp)
target_include_directories(test_http_startup_failfast PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_http_startup_failfast PRIVATE
    GTest::gtest GTest::gtest_main
    Crow::Crow
    Boost::system)
if(WIN32)
    target_link_libraries(test_http_startup_failfast PRIVATE Ws2_32)
endif()
add_test(NAME HttpStartupFailFastTests COMMAND test_http_startup_failfast)

# ── DLL search path for ctest on Windows ──────────────────────────────────────
# Qt/Boost/FFmpeg DLLs are auto-copied next to vms_backend.exe in $<CONFIG>/.
# Modular OpenCV libs (opencv_core4.dll, opencv_imgproc4.dll, ...) live in the
# vcpkg manifest install dir and are NOT copied next to test exes. Prepend BOTH
# the main-binary directory AND vcpkg_installed/<triplet>/bin so the loader can
# find every transitive DLL when ctest launches a test from a non-deploy dir.
#
# PR-7 (2026-05-25): added QT_PLUGIN_PATH so DB-backed integration tests can
# load the QSQLITE driver — without it, QSqlDatabase::open() SEH-crashes
# during init (no qsqlite.dll on the search path, addDatabase returns an
# invalid handle, open() deref crashes). The plugin path is the Qt6/plugins
# root from vcpkg — Qt scans subdirs (sqldrivers/, etc.) automatically.
#
# Use \; to escape the CMake list separator inside a string value.
if(WIN32)
    set(_VMS_VCPKG_BIN     "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin")
    # Qt plugins must match build config (debug DLLs are 'd'-suffixed and
    # depend on Qt6Xxxd.dll; mixing release plugins into a debug exe SEH-
    # crashes inside the driver loader). vcpkg ships separate dirs:
    #   release → vcpkg_installed/x64-windows/Qt6/plugins
    #   debug   → vcpkg_installed/x64-windows/debug/Qt6/plugins
    set(_VMS_QT_PLUGIN_DIR
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<IF:$<CONFIG:Debug>,debug/Qt6/plugins,Qt6/plugins>")
    set(_VMS_TEST_PATH_ENV
        "PATH=${CMAKE_BINARY_DIR}/$<CONFIG>\;${_VMS_VCPKG_BIN}\;$ENV{PATH}"
        "QT_PLUGIN_PATH=${_VMS_QT_PLUGIN_DIR}")
    set_tests_properties(
        AuthTests
        PasswordHashTests
        ReadinessStateTests
        StorageBackoffTests
        HstsHeaderTests
        MetricsAuthTests
        ConfigValidationTests
        CsvEscapeTests
        ApiUtilsSafeErrorTests
        DbPortabilityTests
        AttendanceDedupTests
        AttendanceShiftTests
        LineCrossingTests
        CounterBucketTests
        CounterSummaryTests
        PpeStatusTests
        AttendanceHealthTests
        CounterIntegrationTests
        PpeIntegrationTests
        AttendanceIntegrationTests
        FrameBusTests
        JwtPurposeTests
        HttpStartupFailFastTests
        PROPERTIES ENVIRONMENT "${_VMS_TEST_PATH_ENV}"
    )
endif()
