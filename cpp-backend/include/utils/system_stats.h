#pragma once

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace vms {
namespace utils {

struct SystemMetrics {
    double cpu_usage_percent;
    double ram_total_gb;
    double ram_used_gb;
    double ram_usage_percent;
    double disk_total_gb;
    double disk_used_gb;
    double disk_usage_percent;
    double gpu_usage_percent;       // -1 if not available
    double gpu_memory_used_mb;      // -1 if not available
    double gpu_memory_total_mb;     // -1 if not available
    long long uptime_seconds;       // System uptime in seconds
    std::string cpu_model;
    double ai_accuracy;
};

class SystemStats {
public:
    static SystemMetrics getMetrics();

private:
    static double getCpuUsage();
    static void getRamUsage(double& total_gb, double& used_gb, double& percent);
    static void getDiskUsage(double& total_gb, double& used_gb, double& percent);
    static void getGpuUsage(double& util_percent, double& mem_used, double& mem_total);
    static long long getUptime();
    static std::string getCpuModel();
};

} // namespace utils
} // namespace vms
