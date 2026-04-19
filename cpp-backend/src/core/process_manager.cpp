#include "core/process_manager.h"

#include "utils/logger.h"

#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vms {
namespace core {

ProcessManager& ProcessManager::getInstance() {
    static ProcessManager instance;
    return instance;
}

void ProcessManager::registerProcess(QProcess* process, std::string tag) {
    if (!process) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
#ifdef _WIN32
    assignToJobObjectLocked(process, tag);
#endif
    processes_[process] = Entry{QPointer<QProcess>(process), std::move(tag)};
}

void ProcessManager::unregisterProcess(QProcess* process) {
    if (!process) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    processes_.erase(process);
}

std::vector<ManagedProcessInfo> ProcessManager::listProcesses() {
    std::vector<ManagedProcessInfo> snapshot;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = processes_.begin(); it != processes_.end();) {
        if (!it->second.process) {
            it = processes_.erase(it);
            continue;
        }

        snapshot.push_back(ManagedProcessInfo{
            it->second.process->processId(),
            it->second.tag
        });
        ++it;
    }

    return snapshot;
}

void ProcessManager::terminateAll(int graceful_timeout_ms) {
    std::vector<Entry> processes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = processes_.begin(); it != processes_.end();) {
            if (!it->second.process) {
                it = processes_.erase(it);
                continue;
            }

            processes.push_back(it->second);
            ++it;
        }
    }

    if (processes.empty()) {
        return;
    }

    LOG_WARN("ProcessManager: terminating {} tracked child processes", processes.size());

    // Send terminate request safely across threads
    for (const auto& entry : processes) {
        if (!entry.process) continue;

        LOG_WARN("ProcessManager: sending terminate to tag={}", entry.tag);
        QMetaObject::invokeMethod(entry.process.data(), "terminate", Qt::QueuedConnection);
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(graceful_timeout_ms);
    
    // Poll state safely - process deletion is handled via QPointer nulling
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_done = true;
        for (const auto& entry : processes) {
            if (entry.process && entry.process->state() != QProcess::NotRunning) {
                all_done = false;
                break;
            }
        }
        if (all_done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Force kill remaining
    for (const auto& entry : processes) {
        if (!entry.process || entry.process->state() == QProcess::NotRunning) {
            continue;
        }

        LOG_ERROR("ProcessManager: forcing kill for tag={}", entry.tag);
        QMetaObject::invokeMethod(entry.process.data(), "kill", Qt::QueuedConnection);
    }
}

void ProcessManager::shutdownAll(int graceful_timeout_ms) {
    terminateAll(graceful_timeout_ms);

#ifdef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    closeJobObjectLocked();
#endif
}

#ifdef _WIN32
void ProcessManager::ensureJobObjectLocked() {
    if (child_job_handle_) {
        return;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        LOG_ERROR("ProcessManager: CreateJobObject failed with error {}", GetLastError());
        return;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job,
                                 JobObjectExtendedLimitInformation,
                                 &info,
                                 sizeof(info))) {
        LOG_ERROR("ProcessManager: SetInformationJobObject failed with error {}", GetLastError());
        CloseHandle(job);
        return;
    }

    child_job_handle_ = job;
    LOG_INFO("ProcessManager: child job object created");
}

void ProcessManager::assignToJobObjectLocked(QProcess* process, const std::string& tag) {
    if (!process) {
        return;
    }

    ensureJobObjectLocked();
    if (!child_job_handle_) {
        return;
    }

    const qint64 pid = process->processId();
    if (pid <= 0) {
        return;
    }

    HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!child) {
        LOG_WARN("ProcessManager: OpenProcess failed for pid={} tag={} error={}",
                 pid, tag, GetLastError());
        return;
    }

    if (!AssignProcessToJobObject(static_cast<HANDLE>(child_job_handle_), child)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ACCESS_DENIED) {
            LOG_WARN("ProcessManager: AssignProcessToJobObject failed for pid={} tag={} error={}",
                     pid, tag, err);
        }
    }

    CloseHandle(child);
}

void ProcessManager::closeJobObjectLocked() {
    if (!child_job_handle_) {
        return;
    }

    CloseHandle(static_cast<HANDLE>(child_job_handle_));
    child_job_handle_ = nullptr;
    LOG_INFO("ProcessManager: child job object closed");
}
#endif

} // namespace core
} // namespace vms
