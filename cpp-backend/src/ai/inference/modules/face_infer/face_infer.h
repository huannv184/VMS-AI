#pragma once
#include "trt_engine.h"
#include <vector>

namespace inference {

class FaceInfer {
public:
    FaceInfer(TrtEngine* engine);
    bool extractFaceFeature(const std::vector<float>& faceImage, std::vector<float>& feature);

private:
    TrtEngine* engine_;
};

} // namespace inference
