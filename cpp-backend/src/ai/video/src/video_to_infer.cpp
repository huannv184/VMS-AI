#include "video/video_to_infer.h"
#include "inference/dummy.h"  // ← QUAN TRỌNG: Include để có định nghĩa InferInput/InferOutput

namespace video {

VideoToInfer::VideoToInfer(std::shared_ptr<inference::InferBase> infer)
    : infer_(std::move(infer)) {}

void VideoToInfer::process(const video::Frame& frame) {
    if (!infer_) return;

    // Sử dụng API chuẩn của inference
    // InferInput và InferOutput là struct rỗng theo dummy.h
    inference::InferInput input{};
    inference::InferOutput output{};

    // Gọi inference - struct rỗng nên không cần map field
    infer_->infer(input, output);
}

} // namespace video