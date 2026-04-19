#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <optional>
#include "database/models.h"

namespace vms {
namespace core {

/**
 * @brief Manages camera snapshots and storage
 * Handles binary JPEG capture and metadata indexing
 */
class SnapshotManager {
public:
    static SnapshotManager& getInstance();

    /**
     * @brief Capture a snapshot from a camera
     * @param camera_id ID of the camera
     * @return Path to the saved image if successful, empty string otherwise
     */
    std::string takeSnapshot(int camera_id);

    /**
     * @brief Get recent snapshots
     * @param camera_id ID of the camera (-1 for all)
     * @param limit Maximum number of snapshots to return
     * @return Vector of Snapshot objects
     */
    std::vector<Snapshot> getRecentSnapshots(int camera_id = -1, int limit = 50);

    /**
     * @brief Save a JPEG buffer as a snapshot
     * @param camera_id ID of the camera
     * @param jpeg_data Pointer to JPEG data
     * @param size Size of JPEG data
     * @param timestamp Optional timestamp (0 for current time)
     * @return Path to the saved image if successful, empty string otherwise
     */
    std::string saveSnapshot(int camera_id, const unsigned char* jpeg_data, size_t size, long long timestamp = 0);

    /**
     * @brief Delete a snapshot by name/path
     */
    bool deleteSnapshot(const std::string& snapshot_id);

private:
    SnapshotManager() = default;
    
    std::string generateFilename(int camera_id, long long timestamp);
    void updateIndex(const Snapshot& snapshot);
    void loadIndex();

    std::mutex mutex_;
    std::vector<Snapshot> snapshots_;
    const std::string storage_path_ = "snapshots/";
};

} // namespace core
} // namespace vms
