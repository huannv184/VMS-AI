#include "face_infer.h"

#include <stdexcept>

namespace inference {

FaceInfer::FaceInfer(TrtEngine* engine) : engine_(engine) {
    if (!engine_) {
        // Better to fail loud here than every call to extractFaceFeature.
        throw std::invalid_argument("FaceInfer requires a non-null TrtEngine");
    }
}

bool FaceInfer::extractFaceFeature(const std::vector<float>& faceImage, std::vector<float>& feature) {
    // BUG-INFER-01 (audit 2026-05-07): pre-fix code discarded the bool return
    // of engine_->infer() and then `feature.resize(512, 0.f)` silently filled
    // partial / failed inferences with zero embeddings — caller saw `true` and
    // a 512-dim all-zero vector that L2-normalises to a degenerate match
    // anchor. Now we propagate the engine result and only enforce the
    // 512-dim contract when the engine succeeded.
    feature.clear();
    if (!engine_->infer(faceImage, feature)) {
        feature.clear();
        return false;
    }
    // ArcFace ResNet-100 outputs exactly 512 floats. If the engine produced a
    // shorter tensor (model swapped without updating this code), treat it as a
    // failure rather than zero-padding to 512 — a partially-zero embedding
    // would silently corrupt similarity scoring.
    if (feature.size() != 512) {
        feature.clear();
        return false;
    }
    return true;
}

} // namespace inference
