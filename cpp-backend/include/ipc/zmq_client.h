#pragma once

#include <string>
#include <memory>
#include <zmq.hpp>
#include <optional>

namespace vms {
namespace ipc {

/**
 * @brief ZMQ client for communication with ai-server
 */
class ZMQClient {
public:
    /**
     * @brief Constructor
     * @param endpoint ZMQ endpoint (e.g., "tcp://127.0.0.1:5555")
     */
    explicit ZMQClient(const std::string& endpoint);
    
    /**
     * @brief Destructor
     */
    ~ZMQClient();
    
    /**
     * @brief Connect to ZMQ endpoint
     */
    bool connect();
    
    /**
     * @brief Disconnect from ZMQ endpoint
     */
    void disconnect();
    
    /**
     * @brief Send message
     */
    bool send(const std::string& message);
    
    /**
     * @brief Receive message with timeout
     * @param timeout_ms Timeout in milliseconds
     */
    std::optional<std::string> receive(int timeout_ms = 1000);

    /**
     * @brief Receive multipart message
     * @return Vector of message parts
     */
    std::vector<std::string> receiveMultipart(int timeout_ms = 1000);
    
    /**
     * @brief Check if connected
     */
    bool isConnected() const { return connected_; }

private:
    std::string endpoint_;
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    bool connected_ = false;
};

} // namespace ipc
} // namespace vms
