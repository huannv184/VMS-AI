#include "reid_infer.h"

namespace inference {

float ReIDInfer::cosineSimilarity(const std::vector<float>& f1, const std::vector<float>& f2) {
    float dot = 0.f, n1 = 0.f, n2 = 0.f;
    for(size_t i=0;i<f1.size();++i){
        dot += f1[i]*f2[i];
        n1 += f1[i]*f1[i];
        n2 += f2[i]*f2[i];
    }
    return dot / (std::sqrt(n1)*std::sqrt(n2) + 1e-6f);
}

} // namespace inference
