#include "core/ffmpeg_process.h"
#include <QThread>
#include "core/process_manager.h"
#include "utils/logger.h"
#include <filesystem>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace vms {
namespace core {

/**
 * @brief Helper to find the project root by searching for landmark files like .sixth or .codex
 */
std::filesystem::path findProjectRoot() {
    char exe_buf[1024];
    #ifdef _WIN32
    GetModuleFileNameA(nullptr, exe_buf, 1024);
    #endif
    std::filesystem::path current = std::filesystem::path(exe_buf).parent_path();
    
    // Search up to 5 levels for the project root
    for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(current / ".sixth") || 
            std::filesystem::exists(current / ".codex") ||
            std::filesystem::exists(current / "cpp-backend")) {
            return current;
        }
        if (current.has_parent_path()) {
            current = current.parent_path();
        } else {
            break;
        }
    }
    return std::filesystem::current_path(); // Fallback to CWD
}

std::string getFfmpegPath() {
    std::filesystem::path root = findProjectRoot();

    // Preferred: explicit override via environment variable
    if (const char* env = std::getenv("VMS_FFMPEG_PATH"); env && *env) {
        std::filesystem::path p(env);
        if (std::filesystem::exists(p)) return p.lexically_normal().string();
    }
    if (const char* env = std::getenv("FFMPEG_PATH"); env && *env) {
        std::filesystem::path p(env);
        if (std::filesystem::exists(p)) return p.lexically_normal().string();
    }
    
    std::vector<std::filesystem::path> checks = {
        root / "resources" / "backend" / "ffmpeg.exe",
        root / "desktop" / "dist" / "win-unpacked" / "resources" / "backend" / "ffmpeg.exe",
        root / "cpp-backend" / "build" / "ffmpeg.exe",
        root / "cpp-backend" / "build" / "Release" / "ffmpeg.exe",
    };
    
    for (const auto& p : checks) {
        if (std::filesystem::exists(p)) {
            return p.lexically_normal().string();
        }
    }
    return "ffmpeg.exe"; 
}

std::string getAiWorkerPath() {
    std::filesystem::path root = findProjectRoot();

    std::vector<std::filesystem::path> checks = {
        root / "resources" / "backend" / "ai_worker_v2.exe",
        root / "desktop" / "dist" / "win-unpacked" / "resources" / "backend" / "ai_worker_v2.exe",
        root / "cpp-backend" / "build" / "ai_worker_v2.exe",
        root / "cpp-backend" / "build" / "Release" / "ai_worker_v2.exe"
    };
    
    for (const auto& p : checks) {
        if (std::filesystem::exists(p)) {
            return p.lexically_normal().string();
        }
    }
    return "ai_worker_v2.exe";
}


FFmpegProcess::FFmpegProcess(QObject* parent) : QObject(parent) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &FFmpegProcess::onReadyReadStandardOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &FFmpegProcess::onReadyReadStandardError);
    connect(&process_, &QProcess::errorOccurred, this, &FFmpegProcess::onErrorOccurred);
    connect(&process_, &QProcess::finished, this, &FFmpegProcess::onFinished);
}

FFmpegProcess::~FFmpegProcess() {
    stop();
#ifdef _WIN32
    if (job_object_) {
        CloseHandle(static_cast<HANDLE>(job_object_));
        job_object_ = nullptr;
    }
#endif
}

bool FFmpegProcess::start(const std::string& command) {
    if (QThread::currentThread() != this->thread()) {
        bool success = false;
        QMetaObject::invokeMethod(this, [this, command, &success]() {
            success = this->start(command);
        }, Qt::BlockingQueuedConnection);
        return success;
    }

    stop();

    std::string modified_cmd = command;
    std::string module_name = "";
    
    // Resolve binary path based on command prefix
    if (command.find("ffmpeg") == 0) {
        module_name = getFfmpegPath();
        if (command.find("ffmpeg.exe") == 0) {
            modified_cmd = "\"" + module_name + "\"" + command.substr(10);
        } else {
            modified_cmd = "\"" + module_name + "\"" + command.substr(6);
        }
    } else if (command.find("ai_worker") == 0) {
         module_name = getAiWorkerPath();
         if (command.find("ai_worker_v2.exe") == 0) {
             modified_cmd = "\"" + module_name + "\"" + command.substr(16);
         } else {
             modified_cmd = "\"" + module_name + "\"" + command.substr(9);
         }
         // Set CWD to the directory containing ai_worker_v2.exe
         // so it can find hardcoded relative paths like models/scrfd_2.5g_bnkps.trt
         auto worker_dir = std::filesystem::path(module_name).parent_path();
         process_.setWorkingDirectory(QString::fromStdString(worker_dir.string()));
         LOG_INFO("FFmpegProcess: AI Worker CWD set to: {}", worker_dir.string());
    }

#ifdef _WIN32
    if (!job_object_) {
        job_object_ = CreateJobObjectW(NULL, NULL);
        if (job_object_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(static_cast<HANDLE>(job_object_), JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        } else {
            LOG_WARN("FFmpegProcess: Failed to create Job Object");
        }
    }

    // Set process to start suspended to completely eliminate the assignment race condition
    if (job_object_) {
        process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_SUSPENDED;
        });
    }
#endif

    process_.startCommand(QString::fromStdString(modified_cmd));
    if (!process_.waitForStarted(3000)) {
        LOG_ERROR("FFmpegProcess: Failed to start command: {}", process_.errorString().toStdString());
        return false;
    }

#ifdef _WIN32
    if (job_object_ && process_.processId() > 0) {
        HANDLE hProcess = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process_.processId()));
        if (hProcess) {
            if (!AssignProcessToJobObject(static_cast<HANDLE>(job_object_), hProcess)) {
                LOG_WARN("FFmpegProcess: Failed to assign process to Job Object (PID: {})", process_.processId());
            }
            CloseHandle(hProcess);
        } else {
            LOG_WARN("FFmpegProcess: Failed to open process handle for PID: {}", process_.processId());
        }

        // Resume the thread after successful job assignment
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hSnapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te;
            te.dwSize = sizeof(te);
            if (Thread32First(hSnapshot, &te)) {
                do {
                    if (te.th32OwnerProcessID == process_.processId()) {
                        HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                        if (hThread) {
                            ResumeThread(hThread);
                            CloseHandle(hThread);
                            break; 
                        }
                    }
                } while (Thread32Next(hSnapshot, &te));
            }
            CloseHandle(hSnapshot);
        }
    }
#endif

    ProcessManager::getInstance().registerProcess(
        &process_, module_name.empty() ? "child-process" : module_name);
    return true;
}

void FFmpegProcess::stop() {
    if (isRunning()) {
        // Step 1: Close stdin → FFmpeg receives EOF and begins graceful shutdown
        //         (flushes muxer, writes index, etc.).
        process_.closeWriteChannel();

        // Step 2: Wait up to 3s for the process to exit on its own.
        //         QProcess::waitForFinished() runs an internal event loop that
        //         pumps readyRead events, so stdout/stderr pipes are drained
        //         automatically — no explicit readAll() needed here.
        //
        //         IMPORTANT: Do NOT call readAllStandardOutput() / readAllStandardError()
        //         from this function.  FFmpegProcess is moved to the main Qt thread
        //         (see startPipeline), so QProcess's pipe buffers are owned by that
        //         thread.  Calling read methods from a different thread (HTTP worker,
        //         PipelineContext destructor) is a Qt threading violation and triggers
        //         undefined behaviour.  The readyReadStandard{Output,Error} signals
        //         and waitForFinished()'s internal event loop handle draining safely.
        if (!process_.waitForFinished(3000)) {
            // Step 3: Graceful signal — SIGTERM / WM_CLOSE.
            process_.terminate();

            // Step 4: Wait another 5s after terminate.
            if (!process_.waitForFinished(5000)) {
                LOG_WARN("[FFMPEG] terminate timed out — forcing kill (PID: {})", process_.processId());
#ifdef _WIN32
                // TerminateJobObject kills the entire process tree atomically.
                if (job_object_) {
                    TerminateJobObject(static_cast<HANDLE>(job_object_), 1);
                }
#endif
                // Step 5: Force kill as last resort.
                process_.kill();
                process_.waitForFinished(1000);
            }
        }
    }
    ProcessManager::getInstance().unregisterProcess(&process_);
}

void FFmpegProcess::kill() {
    if (isRunning()) {
#ifdef _WIN32
        if (job_object_) {
            TerminateJobObject(static_cast<HANDLE>(job_object_), 1);
        }
#endif
        process_.kill();
        process_.waitForFinished(1000);
    }
    ProcessManager::getInstance().unregisterProcess(&process_);
}

int FFmpegProcess::wait(int ms) {
    if (process_.waitForFinished(ms)) {
        return process_.exitCode();
    }
    return -1;
}

bool FFmpegProcess::isRunning() {
    return process_.state() == QProcess::Running;
}

qint64 FFmpegProcess::processId() const {
    return process_.processId();
}

int FFmpegProcess::writeStdin(const void* buffer, int buffer_size) {
    if (isRunning()) {
        qint64 written = process_.write(static_cast<const char*>(buffer), buffer_size);
        return static_cast<int>(written);
    }
    return -1;
}

void FFmpegProcess::onReadyReadStandardOutput() {
    QByteArray data = process_.readAllStandardOutput();
    if (!data.isEmpty()) {
        Q_EMIT stdoutReady(data);
    }
}

void FFmpegProcess::onReadyReadStandardError() {
    QByteArray data = process_.readAllStandardError();
    if (!data.isEmpty()) {
        Q_EMIT stderrReady(data);
    }
}

void FFmpegProcess::onErrorOccurred(QProcess::ProcessError error) {
    Q_EMIT processError(process_.errorString());
}

void FFmpegProcess::onFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    ProcessManager::getInstance().unregisterProcess(&process_);
    Q_EMIT processStopped(exitCode);
}

} // namespace core
} // namespace vms
