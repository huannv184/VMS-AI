#pragma once

#include "database/models.h"
#include <vector>
#include <optional>

namespace vms {
namespace database {

class ROIRepository {
public:
    ROIRepository() = default;

    std::vector<ROI> getAllROIs();
    std::vector<ROI> getCameraROIs(int camera_id);
    std::optional<ROI> getROIById(int id);
    // Returns inserted row ID (>0) on success, -1 on failure
    int insertROI(const ROI& roi);
    bool updateROI(const ROI& roi);
    bool deleteROI(int id);
};

} // namespace database
} // namespace vms
