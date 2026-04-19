#include "utils/system_stats.h"
#include "utils/logger.h"

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <sysinfoapi.h>
#else
#include <sys/sysinfo.h>
#endif

#include <iostream>
#include <string>
#include <cmath>
#include <vector>

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
static ULARGE_INTEGER last_cpu_idle_time = {0};
static ULARGE_INTEGER last_cpu_kernel_time = {0};
static ULARGE_INTEGER last_cpu_user_time = {0};
static bool cpu_initialized = false;

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
    return 0.0; // TODO Linux
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
    total_gb = 0; used_gb = 0; percent = 0;
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
    total_gb = 0; used_gb = 0; percent = 0;
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
    return "Unknown Linux CPU"; // Could read /proc/cpuinfo
#endif
}

} // namespace utils
} // namespace vms
