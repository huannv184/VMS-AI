// crow_test_helpers.h — primitives for Crow-level integration tests.
//
// What this gives you:
//   - `RunningCrowApp<App>` : RAII spin-up of a `crow::App<...>` on `127.0.0.1`
//                             and an OS-assigned port. Wraps `app.run_async()`
//                             + `wait_for_server_start()`. Destructor stops
//                             the server and joins the future. Captures any
//                             exception thrown by `run()` and re-throws on
//                             `joinAndPropagate()`.
//   - `pickFreePort()`      : ask the OS for an unused TCP port (helper for
//                             tests that bind two apps and need a known port).
//
// Why a header-only helper:
//   The tests that use this scaffolding all link Crow + Boost::system already.
//   A separate compiled TU would just be one more thing to keep in sync. If
//   the helper grows past ~150 lines or starts depending on httplib for an
//   HTTP client, split it into a .cpp and add a `crow_test_helpers` static lib
//   in tests/CMakeLists.txt.
//
// Out of scope (intentionally):
//   - HTTP client. The first batch of integration tests verifies STARTUP
//     behaviour (BUG-HTTP-01), not request/response cycles. If/when a request
//     test lands, add `httplib::Client` here behind another header — Crow's
//     embedded http_parser is not exposed for outbound use.
//   - JWT/cookie minting. Auth-bypass for tests will be a separate helper
//     once a controller-level test needs it.

#pragma once

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <crow.h>

namespace vms::test {

// Ask the kernel for a TCP port nobody else is using. Caveat: there is a TOCTOU
// window between this call returning and the test binding it. Acceptable for
// CI; if it bites, switch to a per-pid port range.
inline int pickFreePort() {
#ifdef _WIN32
    static thread_local bool wsa_initialised = false;
    if (!wsa_initialised) {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
        wsa_initialised = true;
    }
    using socket_t = SOCKET;
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;
#else
    using socket_t = int;
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
#endif
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;  // let kernel pick
    int port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        sockaddr_in actual{};
#ifdef _WIN32
        int alen = sizeof(actual);
#else
        socklen_t alen = sizeof(actual);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &alen) == 0) {
            port = ntohs(actual.sin_port);
        }
    }
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
    return port;
}

// RAII guard around `app.run_async()`. Construct it AFTER you've registered
// every CROW_ROUTE you want — the app is configured (bindaddr/port/concurrency)
// and asked to start in the constructor body.
//
// Usage:
//   crow::SimpleApp app;
//   CROW_ROUTE(app, "/ping")([]{ return "pong"; });
//   vms::test::RunningCrowApp<crow::SimpleApp> guard(app, port);
//   ASSERT_TRUE(guard.startedWithin(std::chrono::seconds(2)));
//   ... hit endpoints ...
//   // guard's destructor stops the app and joins the future.
template <typename AppT>
class RunningCrowApp {
public:
    RunningCrowApp(AppT& app, int port,
                   const std::string& bindaddr = "127.0.0.1",
                   int concurrency = 1)
        : app_(&app) {
        app.bindaddr(bindaddr).port(static_cast<uint16_t>(port))
           .concurrency(static_cast<uint16_t>(concurrency))
           .loglevel(crow::LogLevel::Critical);
        future_ = app.run_async();
    }

    ~RunningCrowApp() {
        if (!app_) return;
        try {
            app_->stop();
        } catch (...) { /* best-effort */ }
        if (future_.valid()) {
            try {
                // Bounded wait to avoid hanging CI if shutdown stalls.
                if (future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
                    future_.get();
                }
            } catch (...) { /* swallow during dtor */ }
        }
    }

    RunningCrowApp(const RunningCrowApp&)            = delete;
    RunningCrowApp& operator=(const RunningCrowApp&) = delete;

    // True if the app's server-started condvar fired before `timeout`.
    bool startedWithin(std::chrono::milliseconds timeout) {
        return app_ && app_->wait_for_server_start(timeout) == std::cv_status::no_timeout;
    }

    // True if `run()` returned (cleanly or with exception) before `timeout`.
    bool exitedWithin(std::chrono::milliseconds timeout) {
        if (!future_.valid()) return false;
        return future_.wait_for(timeout) == std::future_status::ready;
    }

    // Block until `run()` returns; re-throws any exception captured by the
    // future. Call this in a test body to assert "the server died for reason X".
    void joinAndPropagate() {
        if (future_.valid()) future_.get();
    }

private:
    AppT* app_{nullptr};
    std::future<void> future_;
};

} // namespace vms::test
