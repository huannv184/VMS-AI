#pragma once
#include <opencv2/opencv.hpp>
#include <cstdint>

// Include guard KHÁC với common/frame.h
#ifndef VIDEO_FRAME_OPENCV_DEFINED
#define VIDEO_FRAME_OPENCV_DEFINED

namespace video {

struct Frame {
    cv::Mat image;        // BGR, CV_8UC3
    int64_t timestamp;    // microseconds
    
    Frame() : timestamp(0) {}
    
    explicit Frame(const cv::Mat& img, int64_t ts = 0)
        : image(img), timestamp(ts) {}
    
    inline bool empty() const {
        return image.empty();
    }
    
    inline void clear() {
        image.release();
        timestamp = 0;
    }
};

} // namespace video

#endif // VIDEO_FRAME_OPENCV_DEFINED