#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <unordered_set>
#include <functional>

#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

namespace vms {
namespace core {

struct DeviceInfo {
    std::string ip;
    int port = 0;
    std::string vendor;      // hikvision, dahua, axis, hanwha, etc.
    std::string username;
    std::string password;
    std::string rtsp_url;
    std::string model;
    std::string mac_address;
    std::string firmware;
    bool is_auth_success = false;
    std::string discovery_method; // "onvif", "sunapi", "ip_scan", "cache"
    
    nlohmann::json to_json() const {
        return {
            {"ip", ip},
            {"port", port},
            {"vendor", vendor},
            {"username", username},
            {"password", password},
            {"rtsp_url", rtsp_url},
            {"model", model},
            {"mac_address", mac_address},
            {"firmware", firmware},
            {"is_auth_success", is_auth_success},
            {"discovery_method", discovery_method}
        };
    }
};

struct ScanConfig {
    std::string network_range; // e.g. "192.168.1.0/24"
    bool full_port_range = false;
    std::vector<int> custom_ports;
    bool enable_brute_force = true;
};

// ─── In-Memory Device Cache ──────────────────────────────────
struct CachedDevice {
    DeviceInfo info;
    std::chrono::steady_clock::time_point discovered_at;
};

class NetworkScanner {
public:
    static NetworkScanner& getInstance();

    // Prevent copy/move
    NetworkScanner(const NetworkScanner&) = delete;
    NetworkScanner& operator=(const NetworkScanner&) = delete;

    std::string startScan(const ScanConfig& config);
    void stopScan(const std::string& scan_id);
    nlohmann::json getScanStatus(const std::string& scan_id);
    nlohmann::json getScanResults(const std::string& scan_id);

    // ─── Cache API ───────────────────────────────────────────
    nlohmann::json getCachedDevicesJson();
    void clearCache();
    size_t getCacheSize();

private:
    NetworkScanner();
    ~NetworkScanner();

    struct ScanSession {
        std::string id;
        ScanConfig config;
        std::string status;    // "running", "completed", "stopped", "failed"
        std::string phase;     // "onvif", "sunapi", "ip_scan", "done"
        int progress;          // 0 - 100
        std::chrono::system_clock::time_point start_time;
        std::vector<DeviceInfo> devices;
        std::atomic<bool> stop_flag{false};
        std::thread worker;
        std::mutex devices_mutex; // separate mutex for devices vector
    };

    std::mutex sessions_mutex_;
    std::map<std::string, std::shared_ptr<ScanSession>> sessions_;

    // ─── Cache ───────────────────────────────────────────────
    static constexpr int CACHE_TTL_SECONDS = 300; // 5 minutes
    std::mutex cache_mutex_;
    std::map<std::string, CachedDevice> device_cache_;
    bool isCached(const std::string& ip);
    void addToCache(const DeviceInfo& dev);
    std::vector<DeviceInfo> getCachedDevices();
    void pruneExpiredCache();

    // ─── Discovery Methods ───────────────────────────────────
    void workerLoop(std::shared_ptr<ScanSession> session);
    
    // Phase 1a: ONVIF WS-Discovery (UDP multicast 239.255.255.250:3702)
    void performWSDiscovery(std::shared_ptr<ScanSession> session);
    
    // Phase 1b: SUNAPI Discovery (UDP broadcast :7701) for Hanwha/Samsung
    void performSunapiDiscovery(std::shared_ptr<ScanSession> session);
    
    // Phase 2: Multi-threaded IP range scan
    void multiThreadScan(std::shared_ptr<ScanSession> session,
                         const std::string& base_ip,
                         const std::vector<int>& ports);
    void scanIpBatch(std::shared_ptr<ScanSession> session,
                     const std::string& base_ip,
                     int start_ip, int end_ip,
                     const std::vector<int>& ports);

    // ─── Helpers ─────────────────────────────────────────────
    bool checkPort(const std::string& ip, int port);
    bool attemptBruteForce(const std::string& ip, int port, DeviceInfo& result);
    bool fetchOnvifStreamUri(const std::string& ip, int port, const std::string& user, const std::string& pass, DeviceInfo& result);
    
    bool isAlreadyDiscovered(std::shared_ptr<ScanSession> session, const std::string& ip);
    void addDevice(std::shared_ptr<ScanSession> session, const DeviceInfo& dev);
};

} // namespace core
} // namespace vms
