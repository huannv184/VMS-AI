#include "core/roi_manager.h"
#include "utils/logger.h"

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

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
    if (!roi_repo_) {
        LOG_ERROR("ROIManager not initialized");
        return false;
    }

    auto roi_opt = roi_repo_->getROIById(roi_id);
    if (!roi_opt) {
        return false;
    }

    const ROI& roi = roi_opt.value();
    if (!roi.enabled || roi.points_json.empty()) {
        return false;
    }

    std::vector<cv::Point2f> polygon;
    try {
        auto points = nlohmann::json::parse(roi.points_json);
        if (!points.is_array() || points.size() < 3) {
            return false;
        }
        polygon.reserve(points.size());
        for (const auto& pt : points) {
            float px = 0.0f, py = 0.0f;
            if (pt.is_object()) {
                px = pt.value("x", 0.0f);
                py = pt.value("y", 0.0f);
            } else if (pt.is_array() && pt.size() >= 2) {
                px = pt[0].get<float>();
                py = pt[1].get<float>();
            } else {
                return false;
            }
            polygon.emplace_back(px, py);
        }
    } catch (const std::exception& e) {
        LOG_WARN("ROIManager: invalid polygon JSON for ROI {}: {}", roi_id, e.what());
        return false;
    }

    // Coordinate-space contract: query point and stored polygon must be in the
    // same space. Header documents (x, y) as normalized [0..1]. If the stored
    // polygon looks like pixel coords (>1) we WARN and reject — fail closed
    // rather than silently producing a wrong-result.
    bool polygon_is_pixel = false;
    for (const auto& p : polygon) {
        if (p.x > 1.5f || p.y > 1.5f) { polygon_is_pixel = true; break; }
    }
    if (polygon_is_pixel) {
        LOG_WARN("ROIManager: ROI {} polygon stored in pixel coords; normalize before isPointInROI()", roi_id);
        return false;
    }

    return cv::pointPolygonTest(polygon, cv::Point2f(x, y), false) >= 0.0;
}

} // namespace core
} // namespace vms
