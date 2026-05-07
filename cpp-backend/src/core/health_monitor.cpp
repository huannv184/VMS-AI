#include "core/health_monitor.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <mutex>

namespace vms::core {

HealthMonitor& HealthMonitor::getInstance() {
    static HealthMonitor instance;
    return instance;
}

void HealthMonitor::start(std::function<void()> tick_callback, int interval_ms) {
    QTimer* timer = nullptr;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        // Idempotent: if the timer is already installed, don't overwrite the
        // callback — that would race with the tick lambda which reads it.
        if (timer_ != nullptr) {
            return;
        }
        if (!QCoreApplication::instance()) {
            return;
        }
        tick_callback_ = std::move(tick_callback);

        timer_ = new QTimer();
        timer_->moveToThread(QCoreApplication::instance()->thread());
        QObject::connect(timer_, &QTimer::timeout, timer_, [this]() {
            // Snapshot the callback under lock, then invoke OUTSIDE the lock so
            // the user callback (which may take its own locks or do I/O) cannot
            // serialize with start()/stop().
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> tick_lock(lifecycle_mutex_);
                cb = tick_callback_;
            }
            if (cb) {
                cb();
            }
        });
        timer = timer_;
    }

    if (QThread::currentThread() == timer->thread()) {
        timer->start(interval_ms);
    } else {
        QMetaObject::invokeMethod(timer, [timer, interval_ms]() {
            timer->start(interval_ms);
        }, Qt::QueuedConnection);
    }
}

void HealthMonitor::stop() {
    QTimer* timer = nullptr;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!timer_) {
            return;
        }
        timer = timer_;
        timer_ = nullptr;
        // Clear callback so any in-flight tick that already snapshotted it runs to
        // completion using its local copy, but no future tick can pick up a stale ref.
        tick_callback_ = nullptr;
    }

    if (QThread::currentThread() == timer->thread()) {
        timer->stop();
        delete timer;
    } else {
        QMetaObject::invokeMethod(timer, [timer]() {
            timer->stop();
            timer->deleteLater();
        }, Qt::BlockingQueuedConnection);
    }
}

void HealthMonitor::registerCamera(int camera_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.registered = true;
    snapshot.permanent_failure = false;
    snapshot.state = CameraState::CONNECTING;
    snapshot.last_error.clear();
    snapshot.last_state_change_ms = steadyNowMs();
}

void HealthMonitor::unregisterCamera(int camera_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    snapshots_.erase(camera_id);
}

void HealthMonitor::updateFrameHeartbeat(int camera_id, uint64_t timestamp_ms) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.last_frame_ts = timestamp_ms;
}

void HealthMonitor::setState(int camera_id, CameraState state, const std::string& last_error) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.state = state;
    snapshot.last_state_change_ms = steadyNowMs();
    if (!last_error.empty()) {
        snapshot.last_error = last_error;
    } else if (state != CameraState::FAILED) {
        snapshot.last_error.clear();
    }
}

void HealthMonitor::clearFailure(int camera_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.permanent_failure = false;
    snapshot.last_error.clear();
}

void HealthMonitor::markPermanentFailure(int camera_id, const std::string& reason) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& snapshot = getOrCreateLocked(camera_id);
    snapshot.permanent_failure = true;
    snapshot.state = CameraState::FAILED;
    snapshot.last_error = reason;
    snapshot.last_state_change_ms = steadyNowMs();
}

std::optional<CameraHealthSnapshot> HealthMonitor::snapshot(int camera_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = snapshots_.find(camera_id);
    if (it == snapshots_.end()) {
        return std::nullopt;
    }
    return it->second;
}

int64_t HealthMonitor::steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

CameraHealthSnapshot& HealthMonitor::getOrCreateLocked(int camera_id) {
    return snapshots_[camera_id];
}

} // namespace vms::core
