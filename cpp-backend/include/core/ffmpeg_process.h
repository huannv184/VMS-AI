#pragma once

#include <string>
#include <vector>
#include <memory>

#include <QObject>
#include <QProcess>
#include <QByteArray>

namespace vms {
namespace core {

class FFmpegProcess : public QObject {
    Q_OBJECT
public:
    explicit FFmpegProcess(QObject* parent = nullptr);
    ~FFmpegProcess() override;

    // Prevent copying
    FFmpegProcess(const FFmpegProcess&) = delete;
    FFmpegProcess& operator=(const FFmpegProcess&) = delete;

    /**
     * Start the FFmpeg/AI process with the given command line string.
     */
    bool start(const std::string& command);

    /**
     * Move the internal QProcess value member to the target thread.
     * QObject::moveToThread() only migrates pointer-based children; this
     * method explicitly migrates the value member process_ as well.
     * Must be called from the current thread of process_ (same thread that
     * created this FFmpegProcess object).
     */
    void moveProcessToThread(QThread* thread) { process_.moveToThread(thread); }

    /**
     * Stop the process (graceful terminate, fallback to kill).
     */
    void stop();

    /**
     * Force kill immediately.
     */
    void kill();

    /**
     * Wait for process to exit.
     * @return Exit code, or -1 on timeout/error.
     */
    int wait(int ms = -1);

    /**
     * Check if process is running.
     */
    bool isRunning();

    /**
     * Get the child process id, or 0 if unavailable.
     */
    qint64 processId() const;

    /**
     * Write data to the child process's stdin.
     */
    int writeStdin(const void* buffer, int buffer_size);

Q_SIGNALS:
    void stdoutReady(const QByteArray& data);
    void stderrReady(const QByteArray& data);
    void processError(const QString& errorString);
    void processStopped(int exitCode);

private Q_SLOTS:
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onErrorOccurred(QProcess::ProcessError error);
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QProcess process_;
    void* job_object_ = nullptr;
};

// Resolve FFmpeg executable path using multiple fallback locations
std::string getFfmpegPath();

// Resolve AI Worker executable path using multiple fallback locations
std::string getAiWorkerPath();

} // namespace core
} // namespace vms
