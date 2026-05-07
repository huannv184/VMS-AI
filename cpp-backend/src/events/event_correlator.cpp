// ==============================================================
// File: src/events/event_correlator.cpp  
// Event Correlator Implementation
// ==============================================================

#include "events/event_correlator.h"
#include "events/zone_manager.h"
#include "utils/logger.h"
#include <algorithm>
#include <cmath>
#include <queue>
#include <set>

#include <QThread>
#include <QTimer>
#include <QMetaObject>

namespace vms::events {

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

EventCorrelator::EventCorrelator() {
    stats_start_time_ = std::chrono::steady_clock::now();
    stats_.period_start = std::chrono::system_clock::now();
    
    worker_thread_ = new QThread();
    this->moveToThread(worker_thread_);
    
    LOG_INFO("[EventCorrelator] Created");
}

EventCorrelator::~EventCorrelator() {
    stop();
    if (worker_thread_) {
        worker_thread_->quit();
        worker_thread_->wait();
        delete worker_thread_;
    }
}

void EventCorrelator::start() {
    if (is_running_) {
        LOG_WARN("[EventCorrelator] Already running");
        return;
    }
    
    is_running_ = true;
    
    QMetaObject::invokeMethod(this, [this]() {
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &EventCorrelator::processBuffer);
        timer_->start(500);
    }, Qt::QueuedConnection);
    
    worker_thread_->start();
    
    LOG_INFO("[EventCorrelator] Started");
}

void EventCorrelator::stop() {
    if (!is_running_) return;
    
    LOG_INFO("[EventCorrelator] Stopping...");
    
    is_running_ = false;
    
    QMetaObject::invokeMethod(this, [this]() {
        if (timer_) {
            timer_->stop();
            delete timer_;
            timer_ = nullptr;
        }
    }, Qt::BlockingQueuedConnection);
    
    LOG_INFO("[EventCorrelator] Stopped");
}

// ============================================================================
// EVENT PROCESSING
// ============================================================================

void EventCorrelator::processEvent(const RawEvent& event) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Assign ID if not set
    RawEvent event_copy = event;
    if (event_copy.event_id == 0) {
        event_copy.event_id = generateEventID();
    }
    
    // Add to buffer
    event_buffer_.push_back(event_copy);
    
    // Maintain buffer size
    while (event_buffer_.size() > max_buffer_size_) {
        event_buffer_.pop_front();
    }
    
    // Update stats
    stats_.total_raw_events++;
    stats_.events_by_type[event.type]++;
    stats_.events_by_severity[event.severity]++;
    if (event.zone_id >= 0) {
        stats_.events_by_zone[event.zone_id]++;
    }
    
    lock.unlock();
    
    // D4: Notify worker thread to process buffer immediately via Qt signal/slot queue
    QMetaObject::invokeMethod(this, "processBuffer", Qt::QueuedConnection);
}

std::vector<CorrelatedEvent> EventCorrelator::getRecentCorrelations(
    std::chrono::seconds lookback
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto cutoff = std::chrono::system_clock::now() - lookback;
    
    std::vector<CorrelatedEvent> result;
    for (const auto& corr : active_correlations_) {
        if (corr.last_seen >= cutoff) {
            result.push_back(corr);
        }
    }
    
    return result;
}

CorrelatedEvent* EventCorrelator::getCorrelation(uint64_t correlation_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& corr : active_correlations_) {
        if (corr.correlation_id == correlation_id) {
            return &corr;
        }
    }
    
    return nullptr;
}

// ============================================================================
// CORRELATION ALGORITHMS
// ============================================================================

bool EventCorrelator::areCorrelated(const RawEvent& e1, const RawEvent& e2) {
    // Must be same event type
    if (e1.type != e2.type) {
        return false;
    }
    
    // Check temporal correlation first (fast)
    if (!temporalCorrelation(e1, e2)) {
        return false;
    }
    
    // Check tracking correlation (same object over time)
    if (trackingCorrelation(e1, e2)) {
        return true;
    }
    
    // Check spatial correlation
    if (spatialCorrelation(e1, e2)) {
        return true;
    }
    
    // Check appearance correlation (expensive)
    if (appearanceCorrelation(e1, e2)) {
        return true;
    }
    
    return false;
}

bool EventCorrelator::spatialCorrelation(const RawEvent& e1, const RawEvent& e2) {
    // Same zone (simple check)
    if (e1.zone_id == e2.zone_id && e1.zone_id >= 0) {
        return true;
    }
    
    // World position distance (more precise)
    if (e1.world_position.x == 0.0f && e1.world_position.y == 0.0f) {
        return false; // No world position data
    }
    
    if (e2.world_position.x == 0.0f && e2.world_position.y == 0.0f) {
        return false;
    }
    
    float dx = e1.world_position.x - e2.world_position.x;
    float dy = e1.world_position.y - e2.world_position.y;
    float distance = std::sqrt(dx * dx + dy * dy);
    
    return distance < spatial_threshold_;
}

bool EventCorrelator::temporalCorrelation(const RawEvent& e1, const RawEvent& e2) {
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        e1.timestamp > e2.timestamp ? 
        e1.timestamp - e2.timestamp : 
        e2.timestamp - e1.timestamp
    );
    
    return diff < temporal_window_;
}

bool EventCorrelator::trackingCorrelation(const RawEvent& e1, const RawEvent& e2) {
    // Same camera, same track ID
    if (e1.camera_id == e2.camera_id && 
        e1.track_id == e2.track_id && 
        e1.track_id >= 0) {
        return true;
    }
    
    // Cross-camera tracking would require person re-ID
    // For now, rely on appearance correlation
    return false;
}

bool EventCorrelator::appearanceCorrelation(const RawEvent& e1, const RawEvent& e2) {
    // Check if metadata contains feature embeddings
    if (!e1.metadata.contains("embedding") || !e2.metadata.contains("embedding")) {
        return false;
    }
    
    try {
        auto emb1 = e1.metadata["embedding"].get<std::vector<float>>();
        auto emb2 = e2.metadata["embedding"].get<std::vector<float>>();
        
        if (emb1.size() != emb2.size() || emb1.empty()) {
            return false;
        }
        
        // Cosine similarity
        float dot = 0.0f, norm1 = 0.0f, norm2 = 0.0f;
        for (size_t i = 0; i < emb1.size(); i++) {
            dot += emb1[i] * emb2[i];
            norm1 += emb1[i] * emb1[i];
            norm2 += emb2[i] * emb2[i];
        }
        
        if (norm1 == 0.0f || norm2 == 0.0f) {
            return false;
        }
        
        float similarity = dot / (std::sqrt(norm1) * std::sqrt(norm2));
        
        // Threshold for same person (typically 0.7-0.9)
        return similarity > 0.75f;
        
    } catch (const std::exception& e) {
        LOG_ERROR("[EventCorrelator] Appearance correlation error: {}", e.what());
        return false;
    }
}

// ============================================================================
// CLUSTERING
// ============================================================================

std::vector<std::vector<RawEvent>> EventCorrelator::clusterEvents(
    const std::vector<RawEvent>& events
) {
    std::vector<std::vector<RawEvent>> clusters;
    std::vector<bool> visited(events.size(), false);
    
    // DBSCAN-style clustering
    for (size_t i = 0; i < events.size(); i++) {
        if (visited[i]) continue;
        
        std::vector<RawEvent> cluster;
        std::queue<size_t> queue;
        
        queue.push(i);
        visited[i] = true;
        
        while (!queue.empty()) {
            size_t idx = queue.front();
            queue.pop();
            cluster.push_back(events[idx]);
            
            // Find all neighbors (correlated events)
            for (size_t j = 0; j < events.size(); j++) {
                if (visited[j]) continue;
                
                if (areCorrelated(events[idx], events[j])) {
                    visited[j] = true;
                    queue.push(j);
                }
            }
        }
        
        if (!cluster.empty()) {
            clusters.push_back(cluster);
        }
    }
    
    return clusters;
}

// ============================================================================
// MERGING
// ============================================================================

CorrelatedEvent EventCorrelator::mergeEvents(const std::vector<RawEvent>& cluster) {
    if (cluster.empty()) {
        return CorrelatedEvent{};
    }
    
    CorrelatedEvent correlated;
    correlated.correlation_id = generateCorrelationID();
    correlated.type = cluster[0].type;
    correlated.event_count = cluster.size();
    
    // Collect source IDs and cameras
    std::set<int> unique_cameras;
    std::set<int> unique_zones;
    
    for (const auto& event : cluster) {
        correlated.source_event_ids.push_back(event.event_id);
        unique_cameras.insert(event.camera_id);
        if (event.zone_id >= 0) {
            unique_zones.insert(event.zone_id);
        }
    }
    
    correlated.camera_ids.assign(unique_cameras.begin(), unique_cameras.end());
    correlated.all_zones.assign(unique_zones.begin(), unique_zones.end());
    
    if (!unique_zones.empty()) {
        correlated.primary_zone_id = *unique_zones.begin();
    }
    
    // Temporal span
    auto min_time = cluster[0].timestamp;
    auto max_time = cluster[0].timestamp;
    
    for (const auto& event : cluster) {
        if (event.timestamp < min_time) min_time = event.timestamp;
        if (event.timestamp > max_time) max_time = event.timestamp;
    }
    
    correlated.first_seen = min_time;
    correlated.last_seen = max_time;
    correlated.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        max_time - min_time
    );
    
    // Average confidence
    float sum_confidence = 0.0f;
    for (const auto& event : cluster) {
        sum_confidence += event.confidence;
    }
    correlated.avg_confidence = sum_confidence / cluster.size();
    
    // Centroid position
    float sum_x = 0.0f, sum_y = 0.0f;
    int count = 0;
    for (const auto& event : cluster) {
        if (event.world_position.x != 0.0f || event.world_position.y != 0.0f) {
            sum_x += event.world_position.x;
            sum_y += event.world_position.y;
            count++;
        }
    }
    
    if (count > 0) {
        correlated.centroid_position.x = sum_x / count;
        correlated.centroid_position.y = sum_y / count;
    }
    
    // Find best snapshot (highest confidence)
    auto best = std::max_element(cluster.begin(), cluster.end(),
        [](const RawEvent& a, const RawEvent& b) {
            return a.confidence < b.confidence;
        });
    
    correlated.best_snapshot_path = best->snapshot_path;
    correlated.best_camera_id = best->camera_id;
    
    // Determine severity (use highest)
    correlated.severity = EventSeverity::LOW;
    for (const auto& event : cluster) {
        if (event.severity > correlated.severity) {
            correlated.severity = event.severity;
        }
    }
    
    // Correlation reason
    if (cluster.size() == 1) {
        correlated.correlation_reason = "single_event";
    } else {
        std::vector<std::string> reasons;
        if (unique_cameras.size() > 1) reasons.push_back("multi_camera");
        if (correlated.duration.count() > 1000) reasons.push_back("temporal");
        if (unique_zones.size() > 0) reasons.push_back("spatial");
        
        correlated.correlation_reason = "";
        for (size_t i = 0; i < reasons.size(); i++) {
            if (i > 0) correlated.correlation_reason += "+";
            correlated.correlation_reason += reasons[i];
        }
    }
    
    return correlated;
}

// ============================================================================
// WORKER THREAD
// ============================================================================

void EventCorrelator::processBuffer() {
    // We will collect new correlations and callbacks to invoke outside the lock
    std::vector<CorrelatedEvent> new_correlations;
    std::vector<CorrelationCallback> local_callbacks;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        local_callbacks = callbacks_;
            
        // Cleanup old correlations (>5 minutes old)
        auto cleanup_cutoff = std::chrono::system_clock::now() - std::chrono::minutes(5);
        active_correlations_.erase(
            std::remove_if(active_correlations_.begin(), active_correlations_.end(),
                [cleanup_cutoff](const CorrelatedEvent& c) {
                    return c.last_seen < cleanup_cutoff;
                }),
            active_correlations_.end()
        );
         
        if (event_buffer_.empty()) {
            stats_.active_correlations = active_correlations_.size();
            return;
        }
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Get recent events for clustering
        std::vector<RawEvent> recent_events;
        auto cutoff = std::chrono::system_clock::now() - temporal_window_;
        
        for (const auto& event : event_buffer_) {
            if (event.timestamp >= cutoff) {
                recent_events.push_back(event);
            }
        }
        
        // Cluster events
        auto clusters = clusterEvents(recent_events);
        
        // Merge each cluster
        for (const auto& cluster : clusters) {
            if (cluster.size() < 1) continue;
            
            auto correlated = mergeEvents(cluster);
            
            // Check if this updates an existing correlation
            bool found = false;
            for (auto& existing : active_correlations_) {
                // Check if any source events match
                for (uint64_t src_id : correlated.source_event_ids) {
                    if (std::find(existing.source_event_ids.begin(),
                                 existing.source_event_ids.end(),
                                 src_id) != existing.source_event_ids.end()) {
                        // Update existing
                        existing.last_seen = correlated.last_seen;
                        existing.event_count = correlated.event_count;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            
            if (!found) {
                // New correlation
                active_correlations_.push_back(correlated);
                stats_.total_correlated_events++;
                new_correlations.push_back(correlated);
            }
        }
        
        stats_.active_correlations = active_correlations_.size();
        
        // Calculate latency
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time
        ).count();
        
        stats_.avg_correlation_latency_ms = latency;
        
        // Calculate reduction percentage
        if (stats_.total_raw_events > 0) {
            stats_.reduction_percentage = 
                (1.0f - float(stats_.total_correlated_events) / stats_.total_raw_events) * 100.0f;
        }
    } // release lock

    // Trigger callbacks outside lock to prevent deadlocks
    for (const auto& correlated : new_correlations) {
        for (auto& callback : local_callbacks) {
            try {
                callback(correlated);
            } catch (const std::exception& e) {
                LOG_ERROR("[EventCorrelator] Callback error: {}", e.what());
            }
        }
    }
}

// ============================================================================
// CONFIGURATION
// ============================================================================

void EventCorrelator::setSpatialThreshold(float meters) {
    spatial_threshold_ = meters;
    LOG_INFO("[EventCorrelator] Spatial threshold set to {} meters", meters);
}

void EventCorrelator::setTemporalWindow(std::chrono::milliseconds ms) {
    temporal_window_ = ms;
    LOG_INFO("[EventCorrelator] Temporal window set to {} ms", ms.count());
}

void EventCorrelator::setMinCorrelationConfidence(float conf) {
    min_confidence_ = conf;
}

void EventCorrelator::setMaxBufferSize(size_t size) {
    max_buffer_size_ = size;
}

EventStatistics EventCorrelator::getStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    EventStatistics stats = stats_;
    stats.period_end = std::chrono::system_clock::now();
    
    return stats;
}

void EventCorrelator::resetStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    stats_ = EventStatistics{};
    stats_.period_start = std::chrono::system_clock::now();
    stats_start_time_ = std::chrono::steady_clock::now();
}

void EventCorrelator::onCorrelation(CorrelationCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(callback);
}

uint64_t EventCorrelator::generateCorrelationID() {
    return next_correlation_id_++;
}

uint64_t EventCorrelator::generateEventID() {
    return next_event_id_++;
}

} // namespace vms::events
