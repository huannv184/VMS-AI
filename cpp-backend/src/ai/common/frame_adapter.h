#pragma once

// Include CẢ HAI định nghĩa
#include "common/frame.h"           // edge::Frame
#include "inference/infer_types.h"  // video::Frame

#include <opencv2/opencv.hpp>

namespace frame_utils {

// Chuyển từ edge::Frame sang video::Frame
inline video::Frame toVideoFrame(const edge::Frame& ef) {
    if (ef.empty()) {
        return video::Frame();
    }
    
    int cv_type = (ef.channels == 1) ? CV_8UC1 : 
                  (ef.channels == 3) ? CV_8UC3 : CV_8UC4;
    
    cv::Mat img(ef.height, ef.width, cv_type, 
                const_cast<uint8_t*>(ef.ptr()));
    
    video::Frame vf;
    vf.image = img.clone();  // Deep copy
    vf.timestamp = ef.pts;
    
    return vf;
}

// Chuyển từ video::Frame sang edge::Frame
inline edge::Frame toEdgeFrame(const video::Frame& vf) {
    if (vf.empty()) {
        return edge::Frame();
    }
    
    edge::Frame ef(vf.image.cols, 
                   vf.image.rows, 
                   vf.image.channels(),
                   vf.timestamp,
                   0);
    
    std::memcpy(ef.ptr(), vf.image.data, ef.bytes());
    
    return ef;
}

} // namespace frame_utils