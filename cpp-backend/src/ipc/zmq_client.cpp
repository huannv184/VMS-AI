#include "ipc/zmq_client.h"
#include "utils/logger.h"
#include <vector>

namespace vms {
namespace ipc {

ZMQClient::ZMQClient(const std::string& endpoint)
    : endpoint_(endpoint) {
    LOG_DEBUG("ZMQClient created for endpoint: {}", endpoint);
}

ZMQClient::~ZMQClient() {
    disconnect();
}

bool ZMQClient::connect() {
    try {
        LOG_INFO("Connecting to ZMQ endpoint: {}", endpoint_);
        
        // Create context
        context_ = std::make_unique<zmq::context_t>(1);
        
        // Create socket (SUB socket for subscribing to ai-server)
        socket_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::sub);
        
        // Set socket options
        socket_->set(zmq::sockopt::rcvhwm, 1);  // High water mark
        socket_->set(zmq::sockopt::subscribe, "");  // Subscribe to all messages
        
        // Connect
        socket_->connect(endpoint_);
        
        connected_ = true;
        LOG_INFO("Connected to ZMQ endpoint: {}", endpoint_);
        
        return true;
        
    } catch (const zmq::error_t& e) {
        LOG_ERROR("ZMQ connection error: {}", e.what());
        connected_ = false;
        return false;
    }
}

void ZMQClient::disconnect() {
    if (socket_) {
        try {
            LOG_INFO("Disconnecting from ZMQ endpoint: {}", endpoint_);
            socket_->close();
        } catch (const zmq::error_t& e) {
            LOG_ERROR("ZMQ disconnect error: {}", e.what());
        }
        socket_.reset();
    }
    
    if (context_) {
        context_.reset();
    }
    
    connected_ = false;
}

bool ZMQClient::send(const std::string& message) {
    if (!connected_ || !socket_) {
        LOG_ERROR("ZMQ not connected");
        return false;
    }
    
    try {
        zmq::message_t zmq_msg(message.data(), message.size());
        auto result = socket_->send(zmq_msg, zmq::send_flags::none);
        
        return result.has_value();
        
    } catch (const zmq::error_t& e) {
        LOG_ERROR("ZMQ send error: {}", e.what());
        return false;
    }
}

std::optional<std::string> ZMQClient::receive(int timeout_ms) {
    if (!connected_ || !socket_) {
        LOG_ERROR("ZMQ not connected");
        return std::nullopt;
    }
    
    try {
        // Set receive timeout
        socket_->set(zmq::sockopt::rcvtimeo, timeout_ms);
        
        zmq::message_t zmq_msg;
        auto result = socket_->recv(zmq_msg, zmq::recv_flags::none);
        
        if (result.has_value() && result.value() > 0) {
            std::string message(static_cast<char*>(zmq_msg.data()), zmq_msg.size());
            return message;
        }
        
        return std::nullopt;
        
    } catch (const zmq::error_t& e) {
        if (e.num() == EAGAIN) {
            // Timeout - not an error
            return std::nullopt;
        }
        
        LOG_ERROR("ZMQ receive error: {}", e.what());
        return std::nullopt;
    }
}

std::vector<std::string> ZMQClient::receiveMultipart(int timeout_ms) {
    if (!connected_ || !socket_) {
        return {};
    }
    
    std::vector<std::string> parts;
    try {
        socket_->set(zmq::sockopt::rcvtimeo, timeout_ms);
        
        zmq::message_t msg;
        
        // Receive first part
        auto res = socket_->recv(msg, zmq::recv_flags::none);
        if (!res.has_value()) return {}; // Timeout or error
        
        parts.emplace_back(std::string(static_cast<char*>(msg.data()), msg.size()));
        
        // Receive subsequent parts
        while (socket_->get(zmq::sockopt::rcvmore)) {
            auto res_more = socket_->recv(msg, zmq::recv_flags::none);
            if (!res_more.has_value()) break;
            parts.emplace_back(std::string(static_cast<char*>(msg.data()), msg.size()));
        }
        
        return parts;
    } catch (const zmq::error_t& e) {
        if (e.num() == EAGAIN) return {};
        LOG_ERROR("ZMQ multipart receive error: {}", e.what());
        return {};
    }
}

} // namespace ipc
} // namespace vms
