#include "TubeManager.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace ai {
namespace synopsis {

TubeManager::TubeManager() {
    // Default config
}

TubeManager::~TubeManager() {
}

void TubeManager::processFrame(long long frameTime, 
                               const std::vector<cv::Rect>& boxes, 
                               const std::vector<int>& classIds, 
                               const cv::Mat& frame) {
    if (boxes.empty()) return;

    // TODO: Implement actual tracking (IOU based for simplicity)
    // For each detection, find if it matches an active tube
    // If matched, append to tube
    // If not matched, start new tube
    
    // Simple greedy matching
    std::vector<bool> matchedBox(boxes.size(), false);
    std::vector<int> lostTubes;

    for (auto& [tubeId, tubeIdx] : activeTubeIndices_) {
        // bool tracked = false; // Removed unused variable
        ObjectTube& tube = tubes_[tubeIdx];
        cv::Rect lastRect = tube.frames.back().rect;

        float bestIoU = 0.0f;
        int bestBoxIdx = -1;

        for (size_t i = 0; i < boxes.size(); ++i) {
            if (matchedBox[i]) continue;
            
            // IoU Calculation
            cv::Rect intersect = lastRect & boxes[i];
            float areaIntersect = intersect.area();
            float areaUnion = lastRect.area() + boxes[i].area() - areaIntersect;
            float iou = (areaUnion > 0) ? (areaIntersect / areaUnion) : 0.0f;

            if (iou > iouThreshold_ && iou > bestIoU) {
                bestIoU = iou;
                bestBoxIdx = (int)i; // Cast size_t to int
            }
        }

        if (bestBoxIdx != -1) {
            // Update tube
            matchedBox[bestBoxIdx] = true;

            // MEMORY FIX: Stride and Max Size Check
            if (tube.frames.size() < maxTubeFrames_) {
                // Only store if passes stride check (simple counter or timestamp based)
                // Using frames.size() as proxy for stride counter
                if (tube.frames.empty() || (frameTime - tube.frames.back().timestamp) >= (1000.0/30.0 * frameStride_)) {
                    
                    ObjectTube::TubeFrame tf;
                    tf.timestamp = frameTime;
                    tf.rect = boxes[bestBoxIdx];
                    
                    // MEMORY FIX: Crop bounds check
                    cv::Rect cropRect = boxes[bestBoxIdx] & cv::Rect(0, 0, frame.cols, frame.rows);
                    if (cropRect.area() > 0) {
                         cv::Mat rawCrop = frame(cropRect);
                         
                         // MEMORY FIX: Downscale if too large
                         if (cropRect.area() > maxCropArea_) {
                             float scale = std::sqrt((float)maxCropArea_ / cropRect.area());
                             cv::resize(rawCrop, tf.crop, cv::Size(), scale, scale, cv::INTER_LINEAR);
                         } else {
                             tf.crop = rawCrop.clone();
                         }
                    }
                    tube.frames.push_back(tf);
                }
            }
            // Always update end time even if we didn't store the frame
            tube.endTime = frameTime;
        } else {
            lostTubes.push_back(tubeId);
        }
    }

    // Process lost tubes (if missing too long, deactivate)
    // For simplicity in this initial version, we deactivate immediately if lost
    // Ideally we keep them "active but missing" for maxDropoutFrames
    for (int id : lostTubes) {
        activeTubeIndices_.erase(id);
    }

    // Create new tubes for unmatched boxes
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (!matchedBox[i]) {
            ObjectTube newTube;
            newTube.id = ++nextTubeId_;
            newTube.classId = (i < classIds.size()) ? classIds[i] : -1;
            newTube.startTime = frameTime;
            newTube.endTime = frameTime;
            newTube.label = "Object " + std::to_string(newTube.id);

            ObjectTube::TubeFrame tf;
            tf.timestamp = frameTime;
            tf.rect = boxes[i];
            
            // MEMORY FIX: Crop bounds & Resize
            cv::Rect cropRect = boxes[i] & cv::Rect(0, 0, frame.cols, frame.rows);
            if (cropRect.area() > 0) {
                cv::Mat rawCrop = frame(cropRect);
                if (cropRect.area() > maxCropArea_) {
                     float scale = std::sqrt((float)maxCropArea_ / cropRect.area());
                     cv::resize(rawCrop, tf.crop, cv::Size(), scale, scale, cv::INTER_LINEAR);
                } else {
                     tf.crop = rawCrop.clone();
                }
            }
            newTube.frames.push_back(tf);
            
            tubes_.push_back(newTube);
            activeTubeIndices_[newTube.id] = (int)tubes_.size() - 1; // Cast size_t to int
        }
    }
}

void TubeManager::finalize() {
    activeTubeIndices_.clear();
    // Remove short tubes
    auto it = std::remove_if(tubes_.begin(), tubes_.end(), 
        [this](const ObjectTube& t) {
            return (t.endTime - t.startTime) < minTubeDuration_; 
        });
    tubes_.erase(it, tubes_.end());
    
    spdlog::info("TubeManager finalized: {} valid tubes found", tubes_.size());
}

} // namespace synopsis
} // namespace ai
