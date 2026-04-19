#pragma once

#include "inference/infer_types.h"

namespace video {

// KHÔNG định nghĩa Frame ở đây - dùng từ inference/infer_types.h

struct Detection {
    cv::Rect bbox;
    std::string label;
    float score;
};

enum class SourceType {
    FILE,
    RTSP,
    CAMERA
};

} // namespace video