#pragma once
#include "inference/infer_base.h"

namespace inference {

class CustomModel : public InferBase {
public:
    explicit CustomModel(const InferConfig& cfg) : InferBase(cfg) {}
    bool init() override { return true; }
    bool infer(const InferInput& input, InferOutput& output) override { return true; }
};

} // namespace inference
