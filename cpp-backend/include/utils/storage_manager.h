#ifndef VMS_UTILS_STORAGE_MANAGER_H
#define VMS_UTILS_STORAGE_MANAGER_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>

#include "utils/config.h"

namespace vms {
namespace utils {

/**
 * @brief S3-compatible Storage Manager using libcurl.
 * Handles uploading recordings and snapshots to MinIO.
 */
class StorageManager {
public:
    static StorageManager& getInstance();

    bool init(const Config::StorageConfig& config);
    void shutdown();

    /**
     * @brief Upload a local file to MinIO.
     * @param local_path Path on disk
     * @param object_key Path in MinIO bucket
     * @return true if successful
     */
    bool uploadFile(const std::string& local_path, const std::string& object_key);
    
    /**
     * @brief Download an object from MinIO.
     * @param object_key Path in MinIO bucket
     * @return Data buffer
     */
    std::vector<char> getObject(const std::string& object_key);

    /**
     * @brief Result of a partial GET. `total_size` is the full object length
     * (parsed from Content-Range), -1 if unknown. `http_code` lets callers
     * distinguish 206 (partial), 200 (server ignored Range, returned full body),
     * 416 (range not satisfiable), other (error).
     */
    struct RangeResult {
        std::vector<char> data;
        long long         total_size{-1};
        long              http_code{0};
    };

    /**
     * @brief Fetch a byte range from a MinIO object.
     * @param object_key Path in MinIO bucket
     * @param start First byte offset (inclusive)
     * @param length Number of bytes to fetch (>0)
     */
    RangeResult getObjectRange(const std::string& object_key, size_t start, size_t length);

    /**
     * @brief Generate a presigned URL for GET access.
     * @param object_key Path in MinIO bucket
     * @param expires_in_sec Lifetime of the URL
     * @return Signed URL string
     */
    std::string getPresignedUrl(const std::string& object_key, int expires_in_sec = 3600);

    /**
     * @brief Check if an object exists in MinIO.
     */
    bool exists(const std::string& object_key);

    /**
     * @brief Delete an object from MinIO.
     */
    bool remove(const std::string& object_key);
    bool ensureBucketsExist();

    /**
     * @brief Check if storage is reachable and buckets are ready.
     */
    bool isAvailable() const { return storage_ready_.load(); }

private:
    StorageManager() = default;
    bool createBucket(const std::string& name, long timeout_ms);
    void startBackgroundInitialization();

    std::string driver_{"local"};
    Config::StorageConfig::MinioConfig config_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> storage_ready_{false};
    std::atomic<bool> background_init_in_progress_{false};
    std::mutex init_mutex_;
    std::thread init_thread_;

    // Helper to generate S3 Signature V4 (simplified for MinIO)
    std::string signRequest(const std::string& method, 
                            const std::string& path, 
                            const std::string& query = "");
};

} // namespace utils
} // namespace vms

#endif // VMS_UTILS_STORAGE_MANAGER_H
