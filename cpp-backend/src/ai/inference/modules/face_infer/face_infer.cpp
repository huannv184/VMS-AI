#include "face_infer.h"

namespace inference {

FaceInfer::FaceInfer(TrtEngine* engine) : engine_(engine) {}

bool FaceInfer::extractFaceFeature(const std::vector<float>& faceImage, std::vector<float>& feature) {
    engine_->infer(faceImage, feature);
    // production: ArcFace output size = 128
    feature.resize(128, 0.f);
    return true;
}

} // namespace inference
