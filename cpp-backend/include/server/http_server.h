#pragma once

#include "server/vms_app.h"
#include "middleware/auth_middleware.h"

namespace vms {
namespace server {

class HttpServer {
public:
    HttpServer(const std::string& host, int port, int threads);
    ~HttpServer();
    
    void run();
    void stop();
    void registerRoutes();

private:
    std::string host_;
    int port_;
    int threads_;
    
    VmsApp app_;
    middleware::AuthMiddleware auth_;
};

} // namespace server
} // namespace vms
