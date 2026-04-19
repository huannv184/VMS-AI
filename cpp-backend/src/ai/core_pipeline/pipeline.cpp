#include "pipeline.h"
#include "inference/modules/trt_engine/trt_engine.h"

namespace inference {

Pipeline::Pipeline(const std::string& engine_path)
{
    engine_ = std::make_unique<TrtEngine>(engine_path);
}

std::vector<BoundingBox> Pipeline::run(const std::vector<float>& input)
{
    std::vector<float> output;
    engine_->infer(input, output);

    // dummy postprocess
    return {};
}

} // namespace inference