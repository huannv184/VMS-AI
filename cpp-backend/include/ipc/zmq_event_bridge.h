#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <functional>
#include <zmq.hpp>
#include <nlohmann/json.hpp>

#include <QObject>
#include <QString>

class QThread;

namespace vms {
namespace ipc {

class ZmqEventBridge : public QObject {
    Q_OBJECT
    
public:
    static ZmqEventBridge& getInstance();

    void start();
    void stop();

    bool isRunning() const { return running_.load(); }

Q_SIGNALS:
    // Emitted when an event arrives (for future Phase C consumers)
    void eventReceived(const QString& type, const QString& raw_json);

private:
    ZmqEventBridge() = default;
    ~ZmqEventBridge();

    // No copy or move
    ZmqEventBridge(const ZmqEventBridge&) = delete;
    ZmqEventBridge& operator=(const ZmqEventBridge&) = delete;

    void run(const std::string& endpoint);
    void dispatchMessage(const std::string& raw_json);

    void handleLpr(const nlohmann::json& msg);
    void handleTrafficCount(const nlohmann::json& msg);
    void handleIntrusion(const nlohmann::json& msg);
    void handleLoitering(const nlohmann::json& msg);
    void handleFire(const nlohmann::json& msg);
    void handleSmoke(const nlohmann::json& msg);
    void handlePPEViolation(const nlohmann::json& msg);

    QThread* thread_{nullptr};
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_requested_{ false };

    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t>  socket_;
};

} // namespace ipc
} // namespace vms
