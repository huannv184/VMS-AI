#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

class QTimer;  // forward declare — no QObject inheritance needed

namespace vms {
namespace utils {

class SystemWatchdog {
public:
    static SystemWatchdog& getInstance();

    // Register a thread with a timeout (default 30s)
    void registerThread(const std::string& name, int timeout_ms = 30000);
    
    // Unregister a thread (e.g., on clean exit)
    void unregisterThread(const std::string& name);
    
    // Update the last "seen" time for a thread
    void pulse(const std::string& name);
    
    // Start the monitoring timer
    void start();
    
    // Stop the monitoring timer
    void stop();

private:
    SystemWatchdog();
    ~SystemWatchdog();

    struct ThreadInfo {
        std::atomic<int64_t> last_pulse_ms;
        int timeout_ms;
        std::string name;
    };

    // Thread-safe map of watched threads
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ThreadInfo>> watched_threads_;
    
    // Monitor via QTimer (replaces std::thread)
    std::atomic<bool> running_{false};
    std::atomic<int> hung_count_{0}; // FIX: was static local in monitorLoop — shared incorrectly
    QTimer* timer_{nullptr};
    void checkThreads();  // Single-shot check called by QTimer
};

} // namespace utils
} // namespace vms
