#pragma once
#include <memory>
#include "inference/infer_types.h"   // video::Frame
#include "inference/infer_base.h"    // InferBase

namespace video {

class VideoToInfer {
public:
    explicit VideoToInfer(std::shared_ptr<inference::InferBase> infer);

    void process(const video::Frame& frame);

private:
    std::shared_ptr<inference::InferBase> infer_;
};

} // namespace video