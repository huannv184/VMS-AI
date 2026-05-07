// test_http_startup_failfast.cpp — regression coverage for BUG-HTTP-01.
//
// Pre-fix behaviour: a duplicate `CROW_ROUTE` (or any throw out of `app.run()`)
// killed the HTTP-server thread silently and the rest of the backend bring-up
// continued, eventually logging "Backend started successfully!" with NO LISTENER
// on port 8000.
//
// Post-fix behaviour (cpp-backend/src/main.cpp + cpp-backend/src/server/http_server.cpp,
// 2026-05-06):
//   - HttpServer drives Crow with `app.run_async()` and exposes the future.
//   - Main waits on `app.wait_for_server_start(5s)`. If that times out AND the
//     future is `ready`, run() returned/threw. We call `future.get()` to
//     re-throw and abort startup instead of pretending we're up.
//
// These tests exercise the Crow primitives that the production fix relies on,
// so a future Crow upgrade that changes `run_async`/`wait_for_server_start`
// semantics fails CI here long before it manifests as a silent prod failure.

#include <chrono>
#include <future>
#include <stdexcept>

#include <gtest/gtest.h>

#include "integration/crow_test_helpers.h"

using namespace std::chrono_literals;

// ─── 1. Duplicate route → run() throws → future captures the exception ──────
//
// Mirrors BUG-HTTP-01 exactly: register the same path twice. Crow detects this
// when `app.run()` does its router validation pass (NOT at CROW_ROUTE time).
TEST(HttpStartupFailFast, DuplicateRouteCausesFutureToReportException) {
    crow::SimpleApp app;
    CROW_ROUTE(app, "/dup")([] { return "first"; });
    CROW_ROUTE(app, "/dup")([] { return "second"; });

    const int port = vms::test::pickFreePort();
    ASSERT_GT(port, 0) << "could not pick a free port";

    vms::test::RunningCrowApp<crow::SimpleApp> guard(app, port);

    // run() should return very fast (no sockets bound, just a registration
    // check). Bound at 2s for headroom under loaded CI.
    EXPECT_TRUE(guard.exitedWithin(2s))
        << "run() did not return — duplicate-route detection regressed?";

    // wait_for_server_start MUST time out (the condvar is never signalled).
    EXPECT_FALSE(guard.startedWithin(100ms))
        << "wait_for_server_start signalled despite run() throwing — "
           "main.cpp's BUG-HTTP-01 guard would falsely log 'started successfully'";

    // joinAndPropagate must re-throw — that's what main.cpp catches to abort.
    EXPECT_THROW(guard.joinAndPropagate(), std::exception);
}

// ─── 2. Normal app path → server signals start, future stays unfinished ─────
//
// Sanity check that `RunningCrowApp` doesn't false-positive: a vanilla app
// should pass `wait_for_server_start` and the future should still be running
// (server is in the accept loop) until we ask it to stop.
TEST(HttpStartupFailFast, HealthyAppStartsAndStopsCleanly) {
    crow::SimpleApp app;
    CROW_ROUTE(app, "/ping")([] { return "pong"; });

    const int port = vms::test::pickFreePort();
    ASSERT_GT(port, 0);

    {
        vms::test::RunningCrowApp<crow::SimpleApp> guard(app, port);
        ASSERT_TRUE(guard.startedWithin(5s))
            << "healthy app failed to start — Crow regression?";
        // Server is up; future must NOT be ready (the accept loop is blocking).
        EXPECT_FALSE(guard.exitedWithin(0ms))
            << "future ready while server should be accepting — "
               "either Crow exited early or wait_for_server_start lied";
    } // dtor stops + joins
}

// ─── 3. Port already in use → second app's run() exits → future captures ────
//
// Reproduces the SECOND BUG-HTTP-01-class failure mode: deploy on a host that
// already has 8000 occupied. Crow's bind() returns an error and the run loop
// exits without ever signalling `server_started_`.
TEST(HttpStartupFailFast, PortAlreadyInUseFailsFast) {
    const int port = vms::test::pickFreePort();
    ASSERT_GT(port, 0);

    crow::SimpleApp first;
    CROW_ROUTE(first, "/a")([] { return "a"; });
    vms::test::RunningCrowApp<crow::SimpleApp> first_guard(first, port);
    ASSERT_TRUE(first_guard.startedWithin(5s)) << "first app must come up to occupy the port";

    crow::SimpleApp second;
    CROW_ROUTE(second, "/b")([] { return "b"; });
    vms::test::RunningCrowApp<crow::SimpleApp> second_guard(second, port);

    // Behaviour observed on Crow 1.x + boost::asio:
    //   - Linux: bind() throws inside acceptor::open/bind → run() exits → future ready, throws.
    //   - Windows: SO_REUSEADDR semantics let the second app bind anyway in some
    //     configurations, in which case wait_for_server_start succeeds and the
    //     future stays unfinished.
    //
    // Either path proves "no silent success": EITHER the future reports the
    // error within a short bound, OR the server actually started (just on a
    // different OS-permitted setup). The thing we'd FAIL on is "server didn't
    // start AND future never reported" — which would be the BUG-HTTP-01 hang.
    const bool exited  = second_guard.exitedWithin(2s);
    const bool started = second_guard.startedWithin(100ms);
    EXPECT_TRUE(exited || started)
        << "second app neither started nor reported failure — "
           "this is the silent-hang regression class";

    if (exited) {
        EXPECT_THROW(second_guard.joinAndPropagate(), std::exception)
            << "run() exited without an exception — bind-failure path lost the error";
    }
}
