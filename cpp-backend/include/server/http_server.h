#pragma once

#include <chrono>
#include <future>

#include "server/vms_app.h"
#include "middleware/auth_middleware.h"

namespace vms {
namespace server {

class HttpServer {
public:
    HttpServer(const std::string& host, int port, int threads);
    ~HttpServer();

    // Blocking — calls Crow's run() on the calling thread. Retained for tests
    // and any caller that wants to drive the event loop directly.
    void run();

    // Non-blocking. Configures Crow, returns a future that resolves when the
    // server's event loop exits (cleanly via stop() or with an exception from
    // bind/run failures). Caller MUST keep the future alive and check it
    // — if the future is ready before stop() was requested, the server died.
    std::future<void> runAsync();

    // Block until Crow signals "server_started_" (post-bind, accept loop active).
    // Returns true on success, false on timeout. Safe to call only after
    // runAsync(). Returning false does NOT mean the server is dead — combine
    // with future.wait_for(0ms) to distinguish "slow startup" from "exited".
    bool waitForStart(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    void stop();
    void registerRoutes();

private:
    std::string host_;
    int port_;
    int threads_;
    bool configured_{false};

    VmsApp app_;
    middleware::AuthMiddleware auth_;
};

} // namespace server
} // namespace vms
