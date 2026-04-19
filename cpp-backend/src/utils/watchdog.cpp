// Thread-safety: SystemWatchdog is thread-safe (internally uses std::mutex).
// All public methods can be called from any thread.
// EXCEPTION: std::cerr is used in the FATAL path because spdlog
// itself may be deadlocked when a hung thread is detected.
#include "utils/watchdog.h"
#include "utils/logger.h"
#include <iostream>  // EXCEPTION: Needed for fatal-path cerr fallback
#include <vector>
#include <thread>    // for std::this_thread::sleep_for in fatal path

#include <QTimer>
#include <QObject>
#include <QCoreApplication>
#include <QMetaObject>

namespace vms {
namespace utils {

SystemWatchdog& SystemWatchdog::getInstance() {
    static SystemWatchdog instance;
    return instance;
}

SystemWatchdog::SystemWatchdog() {
    // NOTE: Don't call start() here — QCoreApplication may not exist yet.
    // The caller (main.cpp or first getInstance()) should call start() explicitly
    // after QCoreApplication is constructed.
}

SystemWatchdog::~SystemWatchdog() {
    stop();
}

void SystemWatchdog::start() {
    if (running_) return;
    running_ = true;

    // BUG 1 FIX: Ensure timer lives on the main Qt thread and start is deferred
    // to when the event loop is running (QCoreApplication::exec()).
    timer_ = new QTimer();
    timer_->moveToThread(QCoreApplication::instance()->thread());
    QObject::connect(timer_, &QTimer::timeout, [this]() { checkThreads(); });
    QMetaObject::invokeMethod(timer_, [this]() {
        timer_->setInterval(1000);
        timer_->start();
    }, Qt::QueuedConnection);

    LOG_INFO("SystemWatchdog started (QTimer, 1s interval, deferred to main thread)");
}

void SystemWatchdog::stop() {
    running_ = false;
    if (timer_) {
        timer_->stop();
        delete timer_;
        timer_ = nullptr;
    }
}

void SystemWatchdog::registerThread(const std::string& name, int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto info = std::make_shared<ThreadInfo>();
    info->name = name;
    info->timeout_ms = timeout_ms;
    
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    info->last_pulse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    
    watched_threads_[name] = info;
    LOG_INFO("[Watchdog] Registered thread: {} (Timeout: {}ms)", name, timeout_ms);
}

void SystemWatchdog::unregisterThread(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    watched_threads_.erase(name);
    LOG_INFO("[Watchdog] Unregistered thread: {}", name);
}

void SystemWatchdog::pulse(const std::string& name) {
    // Optimization: Lock-free update of atomic
    // But we need to find the shared_ptr first.
    // If the map doesn't change often, this search is fast. 
    // Ideally we'd return a handle, but by name is safer for decoupled caller.
    
    std::shared_ptr<ThreadInfo> info = nullptr;
    {
        // Short lock just to get pointer
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = watched_threads_.find(name);
        if (it != watched_threads_.end()) {
            info = it->second;
        }
    }
    
    if (info) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        info->last_pulse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }
}

void SystemWatchdog::checkThreads() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    
    std::vector<std::string> hung_threads;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& pair : watched_threads_) {
            auto info = pair.second;
            int64_t last = info->last_pulse_ms;
            
            if ((now_ms - last) > info->timeout_ms) {
                hung_threads.push_back(info->name);
            }
        }
    }
    
    if (!hung_threads.empty()) {
        for (const auto& name : hung_threads) {
            LOG_CRITICAL("[Watchdog] FATAL: Thread '{}' is HUNG! Triggering graceful shutdown.", name);
            std::cerr << "[Watchdog] FATAL: Thread '" << name << "' is HUNG!" << std::endl;
        }
        
        hung_count_++;
        
        // C4 FIX: Do NOT call std::abort() directly — it prevents log/DB flush and
        // can corrupt SQLite. Instead:
        // 1. Log the critical failure (already done above)
        // 2. Stop the timer to prevent re-entry
        // 3. Sleep briefly to allow async log sinks to flush
        // 4. Use std::terminate() which respects registered terminate handlers
        running_ = false;
        if (timer_) timer_->stop();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cerr << "[Watchdog] Triggering std::terminate() after " << hung_count_ << " hung detection(s)." << std::endl;
        std::terminate();
    }
}

} // namespace utils
} // namespace vms
