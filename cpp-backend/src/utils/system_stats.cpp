#include "utils/system_stats.h"
#include "utils/logger.h"

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <sysinfoapi.h>
#else
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#endif

#include <iostream>
#include <string>
#include <cmath>
#include <vector>
#include <fstream>
#include <sstream>

#pragma comment(lib, "pdh.lib")

namespace vms {
namespace utils {

#ifdef _WIN32
namespace {

bool resolveExecutablePath(const wchar_t* executable_name, std::wstring& resolved_path) {
    const DWORD required_size = SearchPathW(nullptr, executable_name, nullptr, 0, nullptr, nullptr);
    if (required_size == 0) {
        return false;
    }

    std::vector<wchar_t> buffer(required_size);
    if (SearchPathW(nullptr, executable_name, nullptr, required_size, buffer.data(), nullptr) == 0) {
        return false;
    }

    resolved_path.assign(buffer.data());
    return true;
}

bool runProcessCaptureStdout(const std::wstring& application_path,
                             const std::wstring& arguments,
                             std::string& output) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return false;
    }

    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return false;
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = write_pipe;
    startup_info.hStdError = write_pipe;

    PROCESS_INFORMATION process_info{};
    std::wstring command_line = L"\"" + application_path + L"\"";
    if (!arguments.empty()) {
        command_line += L" ";
        command_line += arguments;
    }

    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    const BOOL created = CreateProcessW(
        application_path.c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup_info,
        &process_info
    );

    CloseHandle(write_pipe);

    if (!created) {
        CloseHandle(read_pipe);
        return false;
    }

    char buffer[256];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        output.append(buffer, bytes_read);
    }

    WaitForSingleObject(process_info.hProcess, 3000);

    CloseHandle(read_pipe);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return !output.empty();
}

} // namespace
#endif

// static helper for CPU
#ifdef _WIN32
static ULARGE_INTEGER last_cpu_idle_time = {0};
static ULARGE_INTEGER last_cpu_kernel_time = {0};
static ULARGE_INTEGER last_cpu_user_time = {0};
static bool cpu_initialized = false;
#else
// /proc/stat columns: user nice system idle iowait irq softirq steal guest guest_nice
// We treat (idle + iowait) as idle and the sum of all columns as total. Same
// convention as `top`. Single-threaded access assumed (matches Windows version).
static unsigned long long last_cpu_total = 0;
static unsigned long long last_cpu_idle  = 0;
static bool cpu_initialized = false;
#endif

SystemMetrics SystemStats::getMetrics() {
    SystemMetrics m;
    m.cpu_usage_percent = getCpuUsage();
    getRamUsage(m.ram_total_gb, m.ram_used_gb, m.ram_usage_percent);
    getDiskUsage(m.disk_total_gb, m.disk_used_gb, m.disk_usage_percent);
    getGpuUsage(m.gpu_usage_percent, m.gpu_memory_used_mb, m.gpu_memory_total_mb);
    m.uptime_seconds = getUptime();
    m.cpu_model = getCpuModel();
    m.ai_accuracy = 96.5; // Default high accuracy for AI
    return m;
}

double SystemStats::getCpuUsage() {
#ifdef _WIN32
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;

    ULARGE_INTEGER idle_time, kernel_time, user_time;
    idle_time.LowPart = idle.dwLowDateTime; idle_time.HighPart = idle.dwHighDateTime;
    kernel_time.LowPart = kernel.dwLowDateTime; kernel_time.HighPart = kernel.dwHighDateTime;
    user_time.LowPart = user.dwLowDateTime; user_time.HighPart = user.dwHighDateTime;

    if (!cpu_initialized) {
        last_cpu_idle_time = idle_time;
        last_cpu_kernel_time = kernel_time;
        last_cpu_user_time = user_time;
        cpu_initialized = true;
        return 0.0;
    }

    ULONGLONG idle_diff = idle_time.QuadPart - last_cpu_idle_time.QuadPart;
    ULONGLONG kernel_diff = kernel_time.QuadPart - last_cpu_kernel_time.QuadPart;
    ULONGLONG user_diff = user_time.QuadPart - last_cpu_user_time.QuadPart;
    ULONGLONG total = kernel_diff + user_diff;

    last_cpu_idle_time = idle_time;
    last_cpu_kernel_time = kernel_time;
    last_cpu_user_time = user_time;

    if (total == 0) return 0.0;
    
    // (Total - Idle) / Total * 100
    // Note: kernel time includes idle time in some contexts, but GetSystemTimes returns separate.
    // Actually: Kernel includes Idle. 
    // Usage = (Total - Idle) / Total works if Total = User + Kernel.
    // Correct formula for GetSystemTimes:
    // sys = kernel + user
    // usage = (sys - idle) / sys * 100
    
    // Formula: busy = (kernel - idle) + user; total = kernel + user;
    ULONGLONG busy = (kernel_diff - idle_diff) + user_diff;
    ULONGLONG total_time = kernel_diff + user_diff;
    
    if (total_time == 0) return 0.0;
    
    double percent = (double)busy / total_time * 100.0;
    return std::max(0.0, std::min(100.0, percent));
#else
    // Linux: parse /proc/stat aggregate `cpu` line. First call returns 0
    // because we have no delta yet (matches Windows behaviour).
    std::ifstream fs("/proc/stat");
    if (!fs) return 0.0;
    std::string label;
    unsigned long long user{0}, nice{0}, system{0}, idle{0}, iowait{0},
                       irq{0}, softirq{0}, steal{0}, guest{0}, guest_nice{0};
    fs >> label >> user >> nice >> system >> idle >> iowait
       >> irq >> softirq >> steal >> guest >> guest_nice;
    if (label != "cpu") return 0.0;

    const unsigned long long idle_all  = idle + iowait;
    const unsigned long long total_all = user + nice + system + idle + iowait +
                                         irq + softirq + steal;

    if (!cpu_initialized) {
        last_cpu_total = total_all;
        last_cpu_idle  = idle_all;
        cpu_initialized = true;
        return 0.0;
    }

    const unsigned long long total_diff = total_all - last_cpu_total;
    const unsigned long long idle_diff  = idle_all  - last_cpu_idle;
    last_cpu_total = total_all;
    last_cpu_idle  = idle_all;

    if (total_diff == 0) return 0.0;
    double percent = (double)(total_diff - idle_diff) / (double)total_diff * 100.0;
    return std::max(0.0, std::min(100.0, percent));
#endif
}

void SystemStats::getRamUsage(double& total_gb, double& used_gb, double& percent) {
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);

    total_gb = memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    double available_gb = memInfo.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
    used_gb = total_gb - available_gb;
    percent = (used_gb / total_gb) * 100.0;
#else
    // Prefer /proc/meminfo MemAvailable — kernel-computed estimate that
    // accounts for page cache that can be reclaimed (sysinfo's freeram does
    // not, so using it would systematically over-report "used"). Fall back
    // to sysinfo if /proc/meminfo cannot be opened.
    total_gb = 0; used_gb = 0; percent = 0;
    std::ifstream mi("/proc/meminfo");
    if (mi) {
        unsigned long long mem_total_kb = 0, mem_available_kb = 0;
        std::string key; unsigned long long val; std::string unit;
        bool have_total = false, have_avail = false;
        while (mi >> key >> val >> unit) {
            if (key == "MemTotal:")        { mem_total_kb = val;     have_total = true; }
            else if (key == "MemAvailable:"){ mem_available_kb = val; have_avail = true; }
            if (have_total && have_avail) break;
        }
        if (have_total && have_avail && mem_total_kb > 0) {
            total_gb = mem_total_kb / (1024.0 * 1024.0);
            double avail_gb = mem_available_kb / (1024.0 * 1024.0);
            used_gb = std::max(0.0, total_gb - avail_gb);
            percent = (used_gb / total_gb) * 100.0;
            return;
        }
    }
    struct sysinfo si;
    if (sysinfo(&si) == 0 && si.totalram > 0) {
        const double mu = static_cast<double>(si.mem_unit ? si.mem_unit : 1);
        total_gb = (si.totalram * mu) / (1024.0 * 1024.0 * 1024.0);
        double free_gb = ((si.freeram + si.bufferram) * mu) / (1024.0 * 1024.0 * 1024.0);
        used_gb = std::max(0.0, total_gb - free_gb);
        percent = (used_gb / total_gb) * 100.0;
    }
#endif
}

void SystemStats::getDiskUsage(double& total_gb, double& used_gb, double& percent) {
#ifdef _WIN32
    ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
    // Check current drive, or hardcode C:? Let's check where the app is running (Current Directory)
    // using NULL checks current drive.
    if (GetDiskFreeSpaceEx(NULL, &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
        total_gb = totalNumberOfBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
        double free_gb = totalNumberOfFreeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
        used_gb = total_gb - free_gb;
        percent = (used_gb / total_gb) * 100.0;
    } else {
        total_gb = 0; used_gb = 0; percent = 0;
    }
#else
    // Linux: statvfs("/") returns root filesystem size + free blocks.
    total_gb = 0; used_gb = 0; percent = 0;
    struct statvfs sv;
    if (statvfs("/", &sv) == 0 && sv.f_blocks > 0) {
        const double bsize = static_cast<double>(sv.f_frsize ? sv.f_frsize : sv.f_bsize);
        total_gb = (sv.f_blocks * bsize) / (1024.0 * 1024.0 * 1024.0);
        double avail_gb = (sv.f_bavail * bsize) / (1024.0 * 1024.0 * 1024.0);
        used_gb = std::max(0.0, total_gb - avail_gb);
        if (total_gb > 0) percent = (used_gb / total_gb) * 100.0;
    }
#endif
}

void SystemStats::getGpuUsage(double& util_percent, double& mem_used, double& mem_total) {
    util_percent = -1; mem_used = -1; mem_total = -1;

#ifdef _WIN32
    std::wstring nvidia_smi_path;
    if (!resolveExecutablePath(L"nvidia-smi.exe", nvidia_smi_path)) {
        return;
    }

    std::string result;
    if (!runProcessCaptureStdout(
            nvidia_smi_path,
            L"--query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits",
            result)) {
        return;
    }

    const size_t line_end = result.find_first_of("\r\n");
    if (line_end != std::string::npos) {
        result = result.substr(0, line_end);
    }

    try {
        const size_t p1 = result.find(',');
        const size_t p2 = result.find(',', p1 + 1);
        if (p1 != std::string::npos && p2 != std::string::npos) {
            util_percent = std::stod(result.substr(0, p1));
            mem_used = std::stod(result.substr(p1 + 1, p2 - p1 - 1));
            mem_total = std::stod(result.substr(p2 + 1));
        }
    } catch (...) {
        // Ignore malformed output from nvidia-smi.
    }
#endif
}

long long SystemStats::getUptime() {
#ifdef _WIN32
    // GetTickCount64 returns milliseconds since system start
    return static_cast<long long>(GetTickCount64() / 1000);
#else
    struct sysinfo si;
    if (sysinfo(&si) == 0) return static_cast<long long>(si.uptime);
    return 0;
#endif
}

std::string SystemStats::getCpuModel() {
#ifdef _WIN32
    HKEY hKey;
    char CPUName[1024];
    DWORD BufSize = sizeof(CPUName);
    DWORD dwType = REG_SZ;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, "ProcessorNameString", NULL, &dwType, (LPBYTE)CPUName, &BufSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(CPUName);
        }
        RegCloseKey(hKey);
    }
    return "Unknown CPU";
#else
    std::ifstream fs("/proc/cpuinfo");
    if (fs) {
        std::string line;
        while (std::getline(fs, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            // Trim trailing whitespace/tabs from key.
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            if (key == "model name" || key == "Hardware") {
                std::string val = line.substr(colon + 1);
                // Trim leading whitespace.
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos) return val.substr(start);
                return val;
            }
        }
    }
    return "Unknown Linux CPU";
#endif
}

} // namespace utils
} // namespace vms
