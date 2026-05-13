#pragma once
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <nlohmann/json.hpp>
#include "database/db_manager.h"
#include "core/camera_types.h"

namespace vms {
namespace core {

class CameraEventService {
public:
    static CameraEventService& getInstance();

    // Start listening to hardware events via ISAPI/CGI
    void startListening(const std::string& cam_id);

    // Stop listening
    void stopListening(const std::string& cam_id);

    void stopAll();

    // Control PTZ
    bool sendPTZCommand(const std::string& cam_id, const std::string& action, double speed);

    // Return a JSON snapshot of the per-camera event-subscription health so
    // operators can distinguish "no events firing because scene is quiet"
    // from "subscription dropped / brand stub / auth failed". Empty object
    // means we have no session for that cam_id (i.e. startListening was
    // never called or stopListening already ran).
    nlohmann::json getStatus(const std::string& cam_id);

private:
    CameraEventService() = default;
    ~CameraEventService();

    // Prevent copy
    CameraEventService(const CameraEventService&) = delete;
    CameraEventService& operator=(const CameraEventService&) = delete;

    enum class SubscriptionState {
        Pending,   // worker started, has not yet entered pullEvents
        Active,    // pullEvents currently looping (events may or may not be arriving)
        Failed,    // pullEvents returned without a graceful stop signal (brand stub or transport error)
        Stopped    // stopListening called or stop_flag set
    };

    struct EventSession {
        std::string cam_id;
        std::atomic<bool> stop_flag{false};
        std::thread worker;

        // Health snapshot — read by getStatus, written by workerLoop. All
        // primitive types are atomic so no extra mutex is needed. The brand
        // string is set once at start and not mutated after, so a plain
        // std::string is safe to read without synchronisation as long as
        // getStatus holds the outer mutex_ (which guards session lifetime).
        std::atomic<int> state{static_cast<int>(SubscriptionState::Pending)};
        std::atomic<long long> started_at{0};       // unix seconds
        std::atomic<long long> last_event_at{0};    // unix seconds, 0 if none yet
        std::atomic<long long> last_attempt_at{0};  // unix seconds of last pullEvents call
        std::atomic<uint64_t> total_events{0};
        std::atomic<uint64_t> reconnect_count{0};
        std::string brand;                          // brand name at start, immutable
    };

    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<EventSession>> sessions_;

    void workerLoop(std::shared_ptr<EventSession> session);

    CameraDiscovery::DiscoveryConfig getCameraConfig(const std::string& cam_id);
    static const char* subscriptionStateName(SubscriptionState s);
};

} // namespace core
} // namespace vms
