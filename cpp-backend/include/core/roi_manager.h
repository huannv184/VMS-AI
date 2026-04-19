#pragma once

#include "database/models.h"
#include "database/roi_repository.h"
#include <memory>
#include <vector>

namespace vms {
namespace core {

/**
 * @brief ROI manager for region of interest management
 */
class ROIManager {
public:
    static ROIManager& getInstance();
    
    bool init();
    
    /**
     * @brief Add new ROI
     * @return Inserted row ID (>0) on success, -1 on failure
     */
    int addROI(const ROI& roi);
    
    /**
     * @brief Update ROI
     */
    bool updateROI(const ROI& roi);
    
    /**
     * @brief Delete ROI
     */
    bool deleteROI(int roi_id);
    
    /**
     * @brief Get all ROIs
     */
    std::vector<ROI> getAllROIs();

    /**
     * @brief Get all ROIs for camera
     */
    std::vector<ROI> getCameraROIs(int camera_id);
    
    /**
     * @brief Get ROI by ID
     */
    std::optional<ROI> getROI(int roi_id);
    
    /**
     * @brief Check if point is inside ROI
     * @param roi_id ROI ID
     * @param x X coordinate (normalized 0-1)
     * @param y Y coordinate (normalized 0-1)
     */
    bool isPointInROI(int roi_id, float x, float y);

private:
    ROIManager() = default;
    ~ROIManager() = default;
    
    ROIManager(const ROIManager&) = delete;
    ROIManager& operator=(const ROIManager&) = delete;
    
    std::unique_ptr<database::ROIRepository> roi_repo_;
};

} // namespace core
} // namespace vms
