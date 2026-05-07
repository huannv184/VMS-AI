#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vms::utils {

// Sliding-window rate limiter for login protection (H4).
//
// Two independent windows enforced simultaneously:
//   - Per-IP:       5 failures / 60s  → 5-min lockout
//   - Per-username: 10 failures / 300s → 15-min lockout
//
// Thread-safe. All methods may be called concurrently from Crow request threads.

class RateLimiter {
public:
    static RateLimiter& getInstance() {
        static RateLimiter instance;
        return instance;
    }

    // Call before attempting to authenticate.
    // Returns true if the request is allowed; false if the key is currently locked out.
    bool isAllowed(const std::string& key, int max_failures, int window_seconds, int lockout_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        evictStale();

        auto& entry = entries_[key];
        const auto now = std::chrono::steady_clock::now();

        if (entry.locked_until > now) {
            return false; // still in lockout
        }

        // Prune attempts outside the sliding window
        const auto cutoff = now - std::chrono::seconds(window_seconds);
        while (!entry.attempts.empty() && entry.attempts.front() < cutoff) {
            entry.attempts.pop_front();
        }

        return static_cast<int>(entry.attempts.size()) < max_failures;
    }

    // Call after a failed authentication attempt.
    void recordFailure(const std::string& key, int max_failures, int window_seconds, int lockout_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = entries_[key];
        const auto now = std::chrono::steady_clock::now();

        entry.attempts.push_back(now);

        // Prune old attempts
        const auto cutoff = now - std::chrono::seconds(window_seconds);
        while (!entry.attempts.empty() && entry.attempts.front() < cutoff) {
            entry.attempts.pop_front();
        }

        if (static_cast<int>(entry.attempts.size()) >= max_failures) {
            entry.locked_until = now + std::chrono::seconds(lockout_seconds);
            entry.attempts.clear(); // reset window after lockout is set
        }
    }

    // Call after a successful authentication — clears failure history for the key.
    void recordSuccess(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(key);
    }

    // Convenience: combined IP+username check for login endpoint.
    // Returns empty string on success, or an error message if throttled.
    std::string checkLogin(const std::string& ip, const std::string& username) {
        if (!isAllowed("ip:" + ip, 5, 60, 300))
            return "Too many login attempts from your IP. Please wait 5 minutes.";
        if (!isAllowed("user:" + username, 10, 300, 900))
            return "Too many failed attempts for this account. Please wait 15 minutes.";
        return {};
    }

    void recordLoginFailure(const std::string& ip, const std::string& username) {
        recordFailure("ip:" + ip,     5,  60,  300);
        recordFailure("user:" + username, 10, 300, 900);
    }

    void recordLoginSuccess(const std::string& ip, const std::string& username) {
        recordSuccess("ip:" + ip);
        recordSuccess("user:" + username);
    }

    size_t getTrackedEntriesCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    RateLimiter() = default;

    struct Entry {
        std::deque<std::chrono::steady_clock::time_point> attempts;
        std::chrono::steady_clock::time_point locked_until{};
    };

    void evictStale() {
        auto now = std::chrono::steady_clock::now();
        // Periodical cleanup: only every 10 minutes, and only if we have enough entries
        if (now - last_eviction_ < std::chrono::minutes(10) && entries_.size() < 1000) {
            return;
        }

        // Full cleanup: entries with no active lockout and no recent attempts
        const auto cutoff = now - std::chrono::hours(24);
        for (auto it = entries_.begin(); it != entries_.end();) {
            const auto& e = it->second;
            const bool idle = e.locked_until < now &&
                              (e.attempts.empty() || e.attempts.back() < cutoff);
            it = idle ? entries_.erase(it) : std::next(it);
        }
        last_eviction_ = now;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::chrono::steady_clock::time_point last_eviction_{};
};

} // namespace vms::utils
