#pragma once

#include "database/models.h"
#include <vector>
#include <optional>
#include <QSqlQuery>

namespace vms {
namespace database {

/**
 * @brief Camera repository for database operations
 */
class CameraRepository {
public:
    CameraRepository() = default;

    /**
     * @brief Get all cameras
     */
    std::vector<Camera> getAllCameras();

    /**
     * @brief Get camera by ID
     */
    std::optional<Camera> getCameraById(int id);

    /**
     * @brief Insert new camera
     */
    bool insertCamera(Camera& camera);

    /**
     * @brief Update existing camera
     */
    bool updateCamera(const Camera& camera);

    /**
     * @brief Delete camera
     */
    bool deleteCamera(int id);

    /**
     * @brief Get active cameras
     */
    std::vector<Camera> getActiveCameras();

    /**
     * @brief Find an existing camera by name or RTSP URL
     */
    std::optional<Camera> findByNameOrRtsp(const std::string& name, const std::string& rtsp_url,
                                           std::optional<int> exclude_id = std::nullopt);

    /**
     * @brief Get cameras filtered by user permissions (site_id and camera list)
     */
    std::vector<Camera> getFilteredCameras(int userId);

private:
    Camera mapRowToCamera(QSqlQuery& query);
};

} // namespace database
} // namespace vms
