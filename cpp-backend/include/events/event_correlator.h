// ==============================================================
// File: include/events/event_correlator.h
// Event Correlation Engine
// ==============================================================

#pragma once

#include "events/event_types.h"
#include <deque>
#include <atomic>
#include <functional>
#include <mutex>

#include <QObject>

class QThread;
class QTimer;

namespace vms::events {

// ============================================================================
// EVENT CORRELATOR (Main Processing Engine)
// ============================================================================

class EventCorrelator : public QObject {
    Q_OBJECT

public:
    EventCorrelator();
    ~EventCorrelator(); // Not overriding QObject::~QObject here is fine, but it will be virtual since QObject has virtual dtor
    
    // Start/stop background processing
    void start();
    void stop();
    bool isRunning() const { return is_running_; }
    
    // Process new raw event
    void processEvent(const RawEvent& event);
    
    // Get correlated events
    std::vector<CorrelatedEvent> getRecentCorrelations(
        std::chrono::seconds lookback = std::chrono::seconds(60)
    );
    
    CorrelatedEvent* getCorrelation(uint64_t correlation_id);
    
    // Configuration
    void setSpatialThreshold(float meters);
    void setTemporalWindow(std::chrono::milliseconds ms);
    void setMinCorrelationConfidence(float conf);
    void setMaxBufferSize(size_t size);
    
    // Statistics
    EventStatistics getStatistics() const;
    void resetStatistics();
    
    // Callbacks
    using CorrelationCallback = std::function<void(const CorrelatedEvent&)>;
    void onCorrelation(CorrelationCallback callback);
    
private Q_SLOTS:
    // Background worker triggered by QTimer
    void processBuffer();

private:
    // Correlation strategies
    bool areCorrelated(const RawEvent& e1, const RawEvent& e2);
    bool spatialCorrelation(const RawEvent& e1, const RawEvent& e2);
    bool temporalCorrelation(const RawEvent& e1, const RawEvent& e2);
    bool trackingCorrelation(const RawEvent& e1, const RawEvent& e2);
    bool appearanceCorrelation(const RawEvent& e1, const RawEvent& e2);
    
    // Clustering
    std::vector<std::vector<RawEvent>> clusterEvents(
        const std::vector<RawEvent>& events
    );
    
    // Merge cluster into correlated event
    CorrelatedEvent mergeEvents(const std::vector<RawEvent>& cluster);
    
    // ID generation
    uint64_t generateCorrelationID();
    uint64_t generateEventID();
    
    // Data structures
    std::deque<RawEvent> event_buffer_;
    std::vector<CorrelatedEvent> active_correlations_;
    
    // Configuration
    float spatial_threshold_{2.0f};              // meters
    std::chrono::milliseconds temporal_window_{5000}; // 5 seconds
    float min_confidence_{0.7f};
    size_t max_buffer_size_{1000};
    
    // Statistics
    mutable EventStatistics stats_;
    mutable std::chrono::steady_clock::time_point stats_start_time_;
    
    // Threading (Phase D)
    QThread* worker_thread_{nullptr};
    QTimer* timer_{nullptr};
    std::atomic<bool> is_running_{false};
    mutable std::mutex mutex_;
    
    // Callbacks
    std::vector<CorrelationCallback> callbacks_;
    
    // ID counters
    std::atomic<uint64_t> next_correlation_id_{1};
    std::atomic<uint64_t> next_event_id_{1};
};

} // namespace vms::events
