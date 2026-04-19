#pragma once

#include <memory>
#include <atomic>
#include <chrono>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace vms {

/**
 * @brief Logging utility using spdlog
 * 
 * Provides centralized logging for the entire application.
 * Supports file and console output with rotating files.
 */
class Logger {
public:
    /**
     * @brief Initialize logger
     * @param log_file Path to log file
     * @param level Log level (trace, debug, info, warn, error, critical)
     * @param max_file_size_mb Maximum size of each log file in MB
     * @param max_files Maximum number of rotating files
     * @param console_output Enable console output
     */
    static void init(
        const std::string& log_file,
        const std::string& level = "info",
        size_t max_file_size_mb = 100,
        size_t max_files = 5,
        bool console_output = true,
        bool file_output = true
    );
    
    /**
     * @brief Get logger instance
     */
    static std::shared_ptr<spdlog::logger> get();
    
    /**
     * @brief Set log level
     */
    static void setLevel(const std::string& level);

    static void shutdown();
    
private:
    static std::shared_ptr<spdlog::logger> logger_;
};

} // namespace vms

// Convenience macros (Global Scope)
#define LOG_TRACE(...)    if (vms::Logger::get()) vms::Logger::get()->trace(__VA_ARGS__)
#define LOG_DEBUG(...)    if (vms::Logger::get()) vms::Logger::get()->debug(__VA_ARGS__)
#define LOG_INFO(...)     if (vms::Logger::get()) vms::Logger::get()->info(__VA_ARGS__)
#define LOG_WARN(...)     if (vms::Logger::get()) vms::Logger::get()->warn(__VA_ARGS__)
#define LOG_ERROR(...)    if (vms::Logger::get()) vms::Logger::get()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) if (vms::Logger::get()) vms::Logger::get()->critical(__VA_ARGS__)

// ── Throttled Logging ──────────────────────────────────────────────────────
// Emits at most one log per INTERVAL_MS milliseconds per call-site.
// Uses a static atomic at each call-site — O(1), lock-free, no mutex.
// Suppressed count is reported on the next emission.
//
// Usage: LOG_THROTTLED_WARN(5000, "DB connection lost: {}", err);
//
#define LOG_THROTTLED_IMPL_(level, interval_ms, ...)                            \
    do {                                                                        \
        static std::atomic<int64_t>  _thr_last_ms_{0};                          \
        static std::atomic<uint64_t> _thr_suppressed_{0};                       \
        int64_t _thr_now_ = std::chrono::duration_cast<                         \
            std::chrono::milliseconds>(                                         \
            std::chrono::steady_clock::now().time_since_epoch()).count();        \
        int64_t _thr_prev_ = _thr_last_ms_.load(std::memory_order_relaxed);     \
        if (_thr_now_ - _thr_prev_ >= static_cast<int64_t>(interval_ms)) {      \
            if (_thr_last_ms_.compare_exchange_strong(_thr_prev_, _thr_now_,     \
                    std::memory_order_acq_rel)) {                               \
                uint64_t _thr_skip_ = _thr_suppressed_.exchange(0,              \
                    std::memory_order_relaxed);                                 \
                if (vms::Logger::get()) {                                        \
                    vms::Logger::get()->level(__VA_ARGS__);                      \
                    if (_thr_skip_ > 0)                                         \
                        vms::Logger::get()->level(                              \
                            "  (+{} similar messages suppressed)", _thr_skip_); \
                }                                                               \
            } else {                                                            \
                _thr_suppressed_.fetch_add(1, std::memory_order_relaxed);       \
            }                                                                   \
        } else {                                                                \
            _thr_suppressed_.fetch_add(1, std::memory_order_relaxed);           \
        }                                                                       \
    } while (0)

#define LOG_THROTTLED_DEBUG(interval_ms, ...)    LOG_THROTTLED_IMPL_(debug,    interval_ms, __VA_ARGS__)
#define LOG_THROTTLED_INFO(interval_ms, ...)     LOG_THROTTLED_IMPL_(info,     interval_ms, __VA_ARGS__)
#define LOG_THROTTLED_WARN(interval_ms, ...)     LOG_THROTTLED_IMPL_(warn,     interval_ms, __VA_ARGS__)
#define LOG_THROTTLED_ERROR(interval_ms, ...)    LOG_THROTTLED_IMPL_(error,    interval_ms, __VA_ARGS__)
#define LOG_THROTTLED_CRITICAL(interval_ms, ...) LOG_THROTTLED_IMPL_(critical, interval_ms, __VA_ARGS__)
