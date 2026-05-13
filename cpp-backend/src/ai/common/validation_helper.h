#pragma once

#include <cmath>
#include <vector>
#include <iostream>
#include "inference/multi_model_infer.h"

namespace validation {

// ============================================================================
// VALIDATION FUNCTIONS
// ============================================================================

inline bool isValidFloat(float value) {
    return !std::isnan(value) && !std::isinf(value);
}

inline bool isValidBBox(float x1, float y1, float x2, float y2) {
    return isValidFloat(x1) && isValidFloat(y1) && 
           isValidFloat(x2) && isValidFloat(y2) &&
           x2 > x1 && y2 > y1 && 
           x1 >= 0.0f && y1 >= 0.0f;
}

inline bool isValidConfidence(float conf) {
    return isValidFloat(conf) && conf >= 0.0f && conf <= 1.0f;
}

inline void clampBBox(float& x1, float& y1, float& x2, float& y2, 
                     int img_width, int img_height) {
    x1 = std::max(0.0f, std::min(x1, static_cast<float>(img_width)));
    y1 = std::max(0.0f, std::min(y1, static_cast<float>(img_height)));
    x2 = std::max(0.0f, std::min(x2, static_cast<float>(img_width)));
    y2 = std::max(0.0f, std::min(y2, static_cast<float>(img_height)));
}

// ============================================================================
// FILTERING FUNCTIONS
// ============================================================================

inline std::vector<inference::BoundingBox> filterAndValidateObjects(
    const std::vector<inference::BoundingBox>& objects,
    float conf_threshold,
    int img_width,
    int img_height
) {
    std::vector<inference::BoundingBox> filtered;
    filtered.reserve(objects.size());
    
    for (const auto& obj : objects) {
        // Validate confidence
        if (!isValidConfidence(obj.score)) {
            std::cerr << "[Validation] Invalid confidence: " << obj.score 
                     << " for " << obj.label << "\n";
            continue;
        }
        
        // Check threshold
        if (obj.score < conf_threshold) {
            continue;
        }
        
        // Validate bbox
        if (!isValidBBox(obj.x1, obj.y1, obj.x2, obj.y2)) {
            std::cerr << "[Validation] Invalid bbox for " << obj.label 
                     << ": (" << obj.x1 << "," << obj.y1 << "," 
                     << obj.x2 << "," << obj.y2 << ")\n";
            continue;
        }
        
        // Create validated copy with clamped coordinates
        inference::BoundingBox validated = obj;
        clampBBox(validated.x1, validated.y1, validated.x2, validated.y2,
                 img_width, img_height);
        
        // Final check after clamping
        if (validated.x2 <= validated.x1 || validated.y2 <= validated.y1) {
            std::cerr << "[Validation] Invalid bbox after clamping\n";
            continue;
        }
        
        filtered.push_back(validated);
    }
    
    return filtered;
}

inline std::vector<inference::FaceDetection> filterAndValidateFaces(
    const std::vector<inference::FaceDetection>& faces,
    int img_width,
    int img_height
) {
    std::vector<inference::FaceDetection> filtered;
    filtered.reserve(faces.size());
    
    for (const auto& face : faces) {
        // Validate confidence
        if (!isValidConfidence(face.confidence)) {
            std::cerr << "[Validation] Invalid face confidence: " 
                     << face.confidence << "\n";
            continue;
        }
        
        // Validate bbox
        if (!isValidBBox(face.x1, face.y1, face.x2, face.y2)) {
            std::cerr << "[Validation] Invalid face bbox: (" 
                     << face.x1 << "," << face.y1 << "," 
                     << face.x2 << "," << face.y2 << ")\n";
            continue;
        }
        
        // Validate landmarks
        bool landmarks_valid = true;
        for (int i = 0; i < 10; i += 2) {
            if (!isValidFloat(face.landmarks.points[i]) || 
                !isValidFloat(face.landmarks.points[i+1])) {
                landmarks_valid = false;
                break;
            }
        }
        
        if (!landmarks_valid) {
            std::cerr << "[Validation] Invalid face landmarks\n";
            continue;
        }
        
        // Create validated copy
        inference::FaceDetection validated = face;
        clampBBox(validated.x1, validated.y1, validated.x2, validated.y2,
                 img_width, img_height);
        
        // Final check
        if (validated.x2 <= validated.x1 || validated.y2 <= validated.y1) {
            continue;
        }
        
        filtered.push_back(validated);
    }
    
    return filtered;
}

// ============================================================================
// STATISTICS
// ============================================================================

struct ValidationStats {
    uint32_t total_objects = 0;
    uint32_t valid_objects = 0;
    uint32_t invalid_conf = 0;
    uint32_t invalid_bbox = 0;
    uint32_t below_threshold = 0;
    
    uint32_t total_faces = 0;
    uint32_t valid_faces = 0;
    uint32_t invalid_face_conf = 0;
    uint32_t invalid_face_bbox = 0;
    
    void print() const {
        std::cerr << "\n[Validation Stats]\n";
        std::cerr << "Objects: " << valid_objects << "/" << total_objects 
                 << " valid (" 
                 << (total_objects > 0 ? (100.0 * valid_objects / total_objects) : 0.0) 
                 << "%)\n";
        std::cerr << "  - Invalid confidence: " << invalid_conf << "\n";
        std::cerr << "  - Invalid bbox: " << invalid_bbox << "\n";
        std::cerr << "  - Below threshold: " << below_threshold << "\n";
        std::cerr << "Faces: " << valid_faces << "/" << total_faces 
                 << " valid (" 
                 << (total_faces > 0 ? (100.0 * valid_faces / total_faces) : 0.0) 
                 << "%)\n";
        std::cerr << "  - Invalid confidence: " << invalid_face_conf << "\n";
        std::cerr << "  - Invalid bbox: " << invalid_face_bbox << "\n";
    }
};

} // namespace validation