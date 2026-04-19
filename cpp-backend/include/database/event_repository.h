#pragma once

#include "database/models.h"
#include <vector>
#include <optional>

namespace vms {
namespace database {

/**
 * @brief Event repository
 */
class EventRepository {
public:
    EventRepository() = default;

    /**
     * @brief Get events with filters
     */
    std::vector<Event> getEvents(int camera_id = -1,
                                 int limit = 100,
                                 int offset = 0,
                                 const std::string& event_type = "",
                                 std::optional<long long> start_time = std::nullopt,
                                 std::optional<long long> end_time = std::nullopt);

    /**
     * @brief Get event by ID
     */
    std::optional<Event> getEventById(const std::string& id);

    /**
     * @brief Insert new event
     */
    bool insertEvent(const Event& event);

    /**
     * @brief Delete event
     */
    bool deleteEvent(const std::string& id);

    /**
     * @brief Update event video path and duration
     */
    bool updateEventVideo(const std::string& id, const std::string& video_path, int duration_sec);

    /**
     * @brief Get events that have video recordings (video_path IS NOT empty)
     */
    std::vector<Event> getRecordingEvents(int camera_id = -1, int limit = 50, int offset = 0);

    /**
     * @brief Get count of events with recordings
     */
    int getRecordingCount(int camera_id = -1);

    /**
     * @brief Get event count
     */
    int getEventCount(int camera_id = -1);

    /**
     * @brief Prune events older than threshold
     */
    bool pruneOld(time_t threshold);
};

} // namespace database
} // namespace vms
