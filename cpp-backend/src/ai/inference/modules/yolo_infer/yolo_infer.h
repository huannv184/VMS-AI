#pragma once
#include "trt_engine.h"
#include <vector>

namespace inference {

struct YoloBBox {
    float x;        // left
    float y;        // top
    float w;
    float h;
    float score;
    int   class_id;

    // Helper static methods
    static float iou(const YoloBBox& a, const YoloBBox& b);
    
    // Parse raw output from TensorRT buffer
    static std::vector<YoloBBox> parse(
        const float* output, 
        int width, 
        int height,
        float conf_threshold,
        float nms_threshold
    );

private:
    static void nms(
        std::vector<YoloBBox>& boxes, 
        float nms_threshold
    );
};

class YoloInfer {
public:
    YoloInfer(
        TrtEngine* engine,
        int input_w,
        int input_h,
        int num_classes,
        float conf_thresh = 0.5f,
        float nms_thresh  = 0.45f
    );

    bool detect(
        const std::vector<float>& input,
        std::vector<YoloBBox>& boxes,
        int orig_w,
        int orig_h
    );

private:
    TrtEngine* engine_;

    int input_w_;
    int input_h_;
    int num_classes_;
    float conf_thresh_;
    float nms_thresh_;

    static float iou(const YoloBBox& a, const YoloBBox& b);
};

} // namespace inference
