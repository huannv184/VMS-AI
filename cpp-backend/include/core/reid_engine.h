// ==============================================================
// File: include/core/reid_engine.h
// Re-ID Engine — Cross-Camera Person Re-Identification
// ==============================================================

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <nlohmann/json.hpp>

namespace vms {
namespace core {

// ============================================================================
// RE-ID ENTRY — A single person observation
// ============================================================================

struct ReIDEntry {
    int global_id{0};               // Cross-camera global person ID
    int camera_id{0};               // Camera where this observation was made
    int track_id{-1};               // Local tracker ID on that camera
    std::vector<float> embedding;   // Appearance feature vector (typically 512-dim)
    std::string thumbnail_path;     // Path to cropped person image
    std::chrono::system_clock::time_point first_seen;
    std::chrono::system_clock::time_point last_seen;
    
    nlohmann::json toJSON() const;
};

// ============================================================================
// RE-ID TRAIL POINT — One camera visit in a person's trail
// ============================================================================

struct ReIDTrailPoint {
    int camera_id{0};
    std::string camera_name;
    std::string thumbnail_path;
    std::chrono::system_clock::time_point enter_time;
    std::chrono::system_clock::time_point exit_time;
    
    nlohmann::json toJSON() const;
};

// ============================================================================
// RE-ID CONFIG
// ============================================================================

struct ReIDConfig {
    bool enabled{true};
    float match_threshold{0.65f};     // Cosine similarity threshold
    int gallery_ttl_sec{1800};        // 30 minutes default
    int max_gallery_size{500};        // Max persons tracked simultaneously
    int embedding_dim{512};           // Feature vector dimension
    std::string model_path;           // Path to ONNX model file
    
    nlohmann::json toJSON() const;
    static ReIDConfig fromJSON(const nlohmann::json& j);
};

// ============================================================================
// RE-ID ENGINE (Singleton)
// ============================================================================

class ReIDEngine {
public:
    static ReIDEngine& getInstance();
    
    // Initialize with model (call once at startup)
    bool init(const std::string& model_path = "");
    bool isInitialized() const { return initialized_.load(); }

    // True once a real producer has called processDetection at least once.
    // The cross-camera ReID feature is not currently fed by the inference
    // pipeline (BUG-REID-DEAD-PIPELINE 2026-05-08); REST endpoints surface
    // this through getStatistics() so empty gallery/search results aren't
    // mistaken for "no people seen yet".
    bool isProducerWired() const { return producer_wired_.load(); }
    
    /**
     * Process a person detection — extract embedding and match
     * @param camera_id Source camera
     * @param track_id Local tracker ID
     * @param person_crop Cropped person image (BGR)
     * @return Global person ID (existing match or new)
     */
    int processDetection(int camera_id, int track_id, const cv::Mat& person_crop);
    
    // Query gallery
    std::vector<ReIDEntry> getActiveGallery();
    std::vector<ReIDTrailPoint> getPersonTrail(int global_id);

    // Look up the cross-camera global_id assigned to a local tracker. Returns
    // -1 if the (camera_id, track_id) pair has not been observed yet (or was
    // pruned past the gallery TTL). Cheap O(1) hash lookup — safe to call
    // from per-frame handlers that want to enrich event metadata with the
    // re-identified global identity without re-running the DNN.
    int lookupGlobalId(int camera_id, int track_id) const;
    
    // Search by image
    std::vector<std::pair<int, float>> searchByImage(const cv::Mat& query_image, int top_k = 5);
    
    // Gallery management
    void clearGallery();
    void pruneExpired();

    // Persistence (added 2026-05-14): without this the cross-camera gallery
    // resets on every backend restart and operator-built identities are
    // lost. loadFromDatabase is called once from init(); saveToDatabase is
    // driven by a 60s flush timer started in init() and one final flush in
    // shutdown(). Entries past gallery_ttl_sec are silently dropped on
    // load so a long downtime doesn't repopulate a stale gallery.
    bool loadFromDatabase();
    bool saveToDatabase();
    // Final flush + stop the persistence timer. Idempotent. Call from main
    // shutdown sequence before DbManager::close().
    void shutdown();
    
    // Configuration
    ReIDConfig getConfig() const;
    void setConfig(const ReIDConfig& config);
    
    // Statistics
    nlohmann::json getStatistics() const;
    
private:
    ReIDEngine() = default;
    ~ReIDEngine() = default;
    ReIDEngine(const ReIDEngine&) = delete;
    ReIDEngine& operator=(const ReIDEngine&) = delete;
    
    // Extract appearance embedding from person crop
    std::vector<float> extractEmbedding(const cv::Mat& person_crop);
    
    // Find best match in gallery
    int findMatch(const std::vector<float>& embedding, float& best_score);
    
    // Cosine similarity
    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    
    // L2 normalize a vector
    static void l2Normalize(std::vector<float>& vec);
    
    // Save thumbnail
    std::string saveThumbnail(int global_id, int camera_id, const cv::Mat& crop);
    
    // DNN model
    cv::dnn::Net net_;
    std::atomic<bool> initialized_{false};
    // Set true only on the first call to processDetection. See header doc on
    // isProducerWired() for context.
    std::atomic<bool> producer_wired_{false};
    
    // Gallery: global_id -> latest entry
    std::unordered_map<int, ReIDEntry> gallery_;
    
    // Trail: global_id -> list of camera visits
    std::unordered_map<int, std::vector<ReIDTrailPoint>> trails_;
    
    // Lookup: "camera_id:track_id" -> global_id
    std::unordered_map<std::string, int> track_to_global_;
    
    // Config
    ReIDConfig config_;
    
    // Stats
    std::atomic<uint64_t> total_processed_{0};
    std::atomic<uint64_t> total_matched_{0};
    std::atomic<uint64_t> total_new_{0};
    
    // ID generation
    std::atomic<int> next_global_id_{1};
    
    // Thread safety
    mutable std::mutex mutex_;

    // Persistence — dirty flag set whenever gallery_/trails_/track_to_global_
    // mutates. Periodic flush thread observes it, writes to DB, clears it.
    // Avoids per-detection DB writes (would be ~30/s under load) while
    // bounding worst-case data loss to one flush interval.
    std::atomic<bool> persistence_dirty_{false};
    std::atomic<bool> persistence_running_{false};
    std::thread persistence_thread_;
    std::condition_variable persistence_cv_;
    std::mutex persistence_cv_mutex_;

    // Heartbeat — surfaced in getStatistics so an operator (or a future
    // healthcheck endpoint) can tell whether the flush thread is alive.
    // A stale last_flush_epoch (older than 2× flush interval) indicates
    // the thread has silently stopped. Zero = no flush has happened yet.
    std::atomic<int64_t> last_flush_epoch_{0};
    std::atomic<int> last_flush_rows_{0};
};

} // namespace core
} // namespace vms
