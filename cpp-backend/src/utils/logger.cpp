#include "utils/logger.h"
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <filesystem>
#include <iostream>
#include <chrono>

namespace vms {

std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;
namespace {
constexpr size_t kAsyncQueueSize = 8192;
constexpr size_t kAsyncWorkerThreads = 1;
constexpr auto kPeriodicFlushInterval = std::chrono::seconds(5);
}

void Logger::init(
    const std::string& log_file,
    const std::string& level,
    size_t max_file_size_mb,
    size_t max_files,
    bool console_output,
    bool file_output
) {
    try {
        // Create logs directory if it doesn't exist
        std::filesystem::path log_path(log_file);
        if (file_output && log_path.has_parent_path()) {
            std::filesystem::create_directories(log_path.parent_path());
        }
        
        spdlog::drop("vms");
        spdlog::init_thread_pool(kAsyncQueueSize, kAsyncWorkerThreads);

        std::vector<spdlog::sink_ptr> sinks;
        
        // Console sink (colored)
        if (console_output) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
            sinks.push_back(console_sink);
        }
        
        // Rotating file sink
        if (file_output) {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file,
                max_file_size_mb * 1024 * 1024,
                max_files
            );
            file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
            sinks.push_back(file_sink);
        }

        if (sinks.empty()) {
            sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
        }
        
        // overrun_oldest: drop oldest log entry when async queue is full.
        // Prevents request threads from blocking on logging under high load.
        // The alternative (block) causes p95 latency spikes when log volume exceeds
        // the async queue capacity (32768 entries).
        logger_ = std::make_shared<spdlog::async_logger>(
            "vms",
            sinks.begin(),
            sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest
        );
        
        // Set log level
        setLevel(level);
        
        // Register as default logger
        spdlog::register_logger(logger_);
        spdlog::set_default_logger(logger_);
        
        // Enable backtrace (last 32 messages)
        logger_->enable_backtrace(32);
        
        // Only flush immediately on error/critical paths to avoid hot-path I/O stalls.
        logger_->flush_on(spdlog::level::err);
        spdlog::flush_every(kPeriodicFlushInterval);
        
        logger_->info("Logger initialized successfully");
        logger_->info("Log file: {}", log_file);
        logger_->info("Log level: {}", level);
        
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        throw;
    }
}

std::shared_ptr<spdlog::logger> Logger::get() {
    if (!logger_) {
        // Create default logger if not initialized
        init("./logs/vms_backend.log", "info", 100, 5, true);
    }
    return logger_;
}

void Logger::setLevel(const std::string& level) {
    if (!logger_) {
        return;
    }
    
    // Convert string to spdlog level
    spdlog::level::level_enum log_level = spdlog::level::info;
    
    if (level == "trace") {
        log_level = spdlog::level::trace;
    } else if (level == "debug") {
        log_level = spdlog::level::debug;
    } else if (level == "info") {
        log_level = spdlog::level::info;
    } else if (level == "warn" || level == "warning") {
        log_level = spdlog::level::warn;
    } else if (level == "error") {
        log_level = spdlog::level::err;
    } else if (level == "critical") {
        log_level = spdlog::level::critical;
    } else if (level == "off") {
        log_level = spdlog::level::off;
    }
    
    logger_->set_level(log_level);
    logger_->debug("Log level set to: {}", level);
}

void Logger::shutdown() {
    if (logger_) {
        logger_->flush();
    }
    spdlog::drop("vms");
    spdlog::shutdown();
    logger_.reset();
}

} // namespace vms
