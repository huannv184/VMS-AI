#pragma once

#include "database/models.h"
#include "database/event_repository.h"
#include <memory>
#include <optional>
#include <string>

namespace vms {
namespace core {

/**
 * @brief Event manager for event detection and management
 */
class EventManager {
public:
    static EventManager& getInstance();
    
    bool init();
    
    /**
     * @brief Create new event
     */
    bool createEvent(const Event& event);
    
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
    std::optional<Event> getEvent(const std::string& event_id);
    
    /**
     * @brief Delete event
     */
    bool deleteEvent(const std::string& event_id);
    
    /**
     * @brief Get event count
     */
    int getEventCount(int camera_id = -1);
    
    /**
     * @brief Generate UUID for event
     */
    static std::string generateEventId();

private:
    EventManager() = default;
    ~EventManager() = default;
    
    EventManager(const EventManager&) = delete;
    EventManager& operator=(const EventManager&) = delete;
    
    std::unique_ptr<database::EventRepository> event_repo_;
};

} // namespace core
} // namespace vms
