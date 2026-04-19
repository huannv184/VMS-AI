// inference/src/infer_factory.cpp
#include "inference/infer_factory.h"
#include "inference/advanced_infer.h"

namespace inference {

std::unique_ptr<InferBase> createInfer(const InferConfig& cfg) {
    return std::make_unique<AdvancedInfer>(cfg);
}

} // namespace inference