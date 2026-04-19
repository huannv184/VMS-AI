#include "yolo_infer.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace inference {

// ================= Constructor =================
YoloInfer::YoloInfer(
    TrtEngine* engine,
    int input_w,
    int input_h,
    int num_classes,
    float conf_thresh,
    float nms_thresh)
    : engine_(engine)
    , input_w_(input_w)
    , input_h_(input_h)
    , num_classes_(num_classes)
    , conf_thresh_(conf_thresh)
    , nms_thresh_(nms_thresh) {
}

// ================= IoU =================
float YoloInfer::iou(const YoloBBox& a, const YoloBBox& b) {
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w);
    float y2 = std::min(a.y + a.h, b.y + b.h);

    if (x1 >= x2 || y1 >= y2) return 0.f;

    float inter = (x2 - x1) * (y2 - y1);
    float uni = a.w * a.h + b.w * b.h - inter;
    return uni <= 0.f ? 0.f : inter / uni;
}

// ================= Detect =================
bool YoloInfer::detect(
    const std::vector<float>& input,
    std::vector<YoloBBox>& boxes,
    int orig_w,
    int orig_h) {

    if (!engine_ || !engine_->isLoaded()) return false;

    boxes.clear();

    // 1. Inference
    std::vector<float> output;
    if (!engine_->infer(input, output)) {
        return false;
    }

    // 2. Decode (YOLOv8 Format: [1, 4+num_classes, 8400])
    // The output is typically transposed: (4 + num_classes) rows, 8400 columns
    int num_anchors = static_cast<int>(output.size()) / (4 + num_classes_);
    if (num_anchors == 0) return false;

    std::vector<YoloBBox> candidates;
    candidates.reserve(100);

    float scale_w = static_cast<float>(orig_w) / input_w_;
    float scale_h = static_cast<float>(orig_h) / input_h_;

    for (int i = 0; i < num_anchors; i++) {
        // Find best class score
        float best_score = 0.0f;
        int best_cls = -1;

        for (int c = 0; c < num_classes_; ++c) {
            float score = output[(4 + c) * num_anchors + i];
            if (score > best_score) {
                best_score = score;
                best_cls = c;
            }
        }

        if (best_score > conf_thresh_) {
            // YOLOv8 output is cx, cy, w, h
            float cx = output[0 * num_anchors + i];
            float cy = output[1 * num_anchors + i];
            float w  = output[2 * num_anchors + i];
            float h  = output[3 * num_anchors + i];

            float x1 = (cx - w / 2.f) * scale_w;
            float y1 = (cy - h / 2.f) * scale_h;
            float rw = w * scale_w;
            float rh = h * scale_h;

            candidates.push_back({
                x1, y1, rw, rh, best_score, best_cls
            });
        }
    }

    // 3. Sort
    std::sort(candidates.begin(), candidates.end(),
              [](const YoloBBox& a, const YoloBBox& b) {
                  return a.score > b.score;
              });

    // 4. NMS
    for (const auto& c : candidates) {
        bool keep = true;
        for (const auto& b : boxes) {
            if (iou(c, b) > nms_thresh_) {
                keep = false;
                break;
            }
        }
        if (keep) {
            boxes.push_back(c);
        }
    }

    return true;
}

} // namespace inference
