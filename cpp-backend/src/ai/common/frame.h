#pragma once
#include <cstdint>
#include <vector>
#include <memory>

// Include guard để tránh xung đột
#ifndef VIDEO_FRAME_DEFINED
#define VIDEO_FRAME_DEFINED

namespace edge {

/**
 * @brief CPU frame structure (IPC-safe)
 */
struct Frame {
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t channels = 0;

    uint64_t pts = 0;
    uint32_t cam_id = 0;

    // raw data (CPU memory)
    std::vector<uint8_t> data;

    Frame() = default;

    Frame(uint32_t w,
          uint32_t h,
          uint32_t c,
          uint64_t timestamp,
          uint32_t camera_id)
        : width(w),
          height(h),
          channels(c),
          pts(timestamp),
          cam_id(camera_id),
          data(w * h * c) {}

    inline size_t bytes() const {
        return data.size();
    }

    inline uint8_t* ptr() {
        return data.data();
    }

    inline const uint8_t* ptr() const {
        return data.data();
    }
    
    inline bool empty() const {
        return data.empty() || width == 0 || height == 0;
    }
    
    inline void clear() {
        data.clear();
        width = height = channels = 0;
        pts = 0;
        cam_id = 0;
    }
};

using FramePtr = std::shared_ptr<Frame>;

} // namespace edge

#endif // VIDEO_FRAME_DEFINED