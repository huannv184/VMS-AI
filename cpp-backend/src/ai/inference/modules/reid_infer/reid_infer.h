#pragma once
#include <vector>
#include <cmath>

namespace inference {

class ReIDInfer {
public:
    static float cosineSimilarity(const std::vector<float>& f1, const std::vector<float>& f2);
};

} // namespace inference
