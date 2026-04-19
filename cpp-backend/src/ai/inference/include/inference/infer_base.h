// inference/include/inference/infer_base.h
#pragma once

#include "infer_config.h"
#include <string>
#include <vector>
#include <cstdint>
#include <opencv2/opencv.hpp>
#include <cstring>

namespace inference {

// ============================================================================
// BOUNDING BOX
// ============================================================================
struct BBox {
    float x1, y1, x2, y2;
    float score;
    int class_id;
    std::string label;
    
    BBox() 
        : x1(0), y1(0), x2(0), y2(0)
        , score(0), class_id(-1)
        , label("") {}
};

// ============================================================================
// FACE STRUCTURES
// ============================================================================
struct FaceLandmark {
    float points[10];  // [x1, y1, x2, y2, x3, y3, x4, y4, x5, y5]
    
    FaceLandmark() {
        std::memset(points, 0, sizeof(points));
    }
};

struct FaceDetection {
    float x1, y1, x2, y2;
    float confidence;
    FaceLandmark landmarks;
    
    FaceDetection() : x1(0), y1(0), x2(0), y2(0), confidence(0) {}
};

// ============================================================================
// INPUT/OUTPUT STRUCTURES
// ============================================================================
struct InferInput {
    cv::Mat image;
    uint32_t frame_id = 0;
    uint64_t timestamp_ms = 0;
    
    InferInput() = default;
    explicit InferInput(const cv::Mat& img, uint32_t id = 0, uint64_t ts = 0)
        : image(img), frame_id(id), timestamp_ms(ts) {}
};

struct InferOutput {
    bool success = false;
    std::vector<BBox> objects;
    std::vector<FaceDetection> faces;
    double inference_time_ms = 0.0;
    
    InferOutput() = default;
};

// ============================================================================
// BASE INTERFACE
// ============================================================================
class InferBase {
public:
    explicit InferBase(const InferConfig& cfg) : config_(cfg) {}
    virtual ~InferBase() = default;

    virtual bool init() = 0;
    virtual bool infer(const InferInput& input, InferOutput& output) = 0;

protected:
    InferConfig config_;
};

} // namespace inference