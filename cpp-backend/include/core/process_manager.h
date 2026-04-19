#pragma once

#include <QPointer>
#include <QProcess>
#include <QtGlobal>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vms {
namespace core {

struct ManagedProcessInfo {
    qint64 pid{0};
    std::string tag;
};

class ProcessManager {
public:
    static ProcessManager& getInstance();

    void registerProcess(QProcess* process, std::string tag);
    void unregisterProcess(QProcess* process);
    std::vector<ManagedProcessInfo> listProcesses();
    void terminateAll(int graceful_timeout_ms = 1500);
    void shutdownAll(int graceful_timeout_ms = 3000);

private:
    ProcessManager() = default;

#ifdef _WIN32
    void ensureJobObjectLocked();
    void assignToJobObjectLocked(QProcess* process, const std::string& tag);
    void closeJobObjectLocked();
    void* child_job_handle_{nullptr};
#endif

    struct Entry {
        QPointer<QProcess> process;
        std::string tag;
    };

    std::mutex mutex_;
    std::unordered_map<QProcess*, Entry> processes_;
};

} // namespace core
} // namespace vms
