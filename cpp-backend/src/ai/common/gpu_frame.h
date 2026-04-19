#pragma once
#include <vector>

namespace vms {
namespace common {

struct GpuFrame {
    std::vector<unsigned char> data;
    int width{0};
    int height{0};
    int64_t timestamp_us{0}; // thêm member timestamp
};

} // namespace common
} // namespace vms
