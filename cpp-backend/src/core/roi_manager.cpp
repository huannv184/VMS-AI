#include "core/roi_manager.h"
#include "utils/logger.h"

namespace vms {
namespace core {

ROIManager& ROIManager::getInstance() {
    static ROIManager instance;
    return instance;
}

bool ROIManager::init() {
    LOG_INFO("Initializing ROIManager...");
    
    roi_repo_ = std::make_unique<database::ROIRepository>();
    
    LOG_INFO("ROIManager initialized");
    return true;
}

int ROIManager::addROI(const ROI& roi) {
    LOG_INFO("Adding ROI: {} for camera {}", roi.name, roi.camera_id);

    if (!roi_repo_) {
        LOG_ERROR("ROIManager not initialized");
        return -1;
    }

    int inserted_id = roi_repo_->insertROI(roi);

    if (inserted_id > 0) {
        LOG_INFO("ROI added successfully: {} (id={})", roi.name, inserted_id);
    } else {
        LOG_ERROR("Failed to add ROI: {}", roi.name);
    }

    return inserted_id;
}

bool ROIManager::updateROI(const ROI& roi) {
    LOG_INFO("Updating ROI: {} (ID: {})", roi.name, roi.id);
    
    if (!roi_repo_) {
        LOG_ERROR("ROIManager not initialized");
        return false;
    }
    
    bool result = roi_repo_->updateROI(roi);
    
    if (result) {
        LOG_INFO("ROI updated successfully: {}", roi.name);
    } else {
        LOG_ERROR("Failed to update ROI: {}", roi.name);
    }
    
    return result;
}

bool ROIManager::deleteROI(int roi_id) {
    LOG_INFO("Deleting ROI ID: {}", roi_id);
    
    if (!roi_repo_) {
        LOG_ERROR("ROIManager not initialized");
        return false;
    }
    
    bool result = roi_repo_->deleteROI(roi_id);
    
    if (result) {
        LOG_INFO("ROI deleted successfully: ID {}", roi_id);
    } else {
        LOG_ERROR("Failed to delete ROI: ID {}", roi_id);
    }
    
    return result;
}

std::vector<ROI> ROIManager::getAllROIs() {
    if (!roi_repo_) {
        LOG_ERROR("ROIManager not initialized");
        return {};
    }
    return roi_repo_->getAllROIs();
}

std::vector<ROI> ROIManager::getCameraROIs(int camera_id) {
    if (!roi_repo_) {
        LOG_ERROR("ROIManager not initialized");
        return {};
    }
    
    return roi_repo_->getCameraROIs(camera_id);
}

std::optional<ROI> ROIManager::getROI(int roi_id) {
    if (!roi_repo_) {
        LOG_ERROR("ROIManager not initialized");
        return std::nullopt;
    }
    
    return roi_repo_->getROIById(roi_id);
}

bool ROIManager::isPointInROI(int roi_id, float x, float y) {
    // TODO: Implement point-in-polygon detection using Boost.Geometry
    // For now, return false
    return false;
}

} // namespace core
} // namespace vms
