#pragma once

#include "models.h"
#include <vector>
#include <string>
#include <ctime>

namespace vms {
namespace database {

/**
 * @brief Repository for recording_segments table
 * Handles CRUD operations for continuous recording segments
 */
class SegmentRepository {
public:
    SegmentRepository() = default;
    ~SegmentRepository() = default;

    /**
     * @brief Insert a new recording segment
     */
    bool insertSegment(int camera_id, const std::string& filename,
                       time_t start_time, time_t end_time, size_t file_size,
                       const std::string& status = "completed");

    /**
     * @brief Get segments for a camera within a time range
     * @param camera_id  Camera ID
     * @param from_ts    Start unix timestamp (0 = no limit)
     * @param to_ts      End unix timestamp (0 = now)
     */
    std::vector<RecordingSegment> getSegments(int camera_id,
                                              time_t from_ts = 0,
                                              time_t to_ts = 0);

    /**
     * @brief Get segment by ID
     */
    RecordingSegment getSegmentById(int segment_id);

    /**
     * @brief Get total segment count for a camera
     */
    int getSegmentCount(int camera_id);

    /**
     * @brief Delete segments older than a given timestamp
     */
    bool deleteOldSegments(time_t before_ts);

    /**
     * @brief Update segment status and file size
     */
    bool updateSegment(int segment_id, const std::string& status, size_t file_size);
};

} // namespace database
} // namespace vms
