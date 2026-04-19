#pragma once
#include <memory>
#include <vector>
#include "video/video_source.h"
#include "common/frame.h"

namespace video {

class VideoPipeline {
public:
    // Add constructor that takes VideoSource
    explicit VideoPipeline(std::unique_ptr<VideoSource> source);
    VideoPipeline() = default;
    ~VideoPipeline() = default;

    // Add open() and close() methods that Main.cpp expects
    bool open();
    void close();
    
    bool read(Frame& frame);

private:
    std::unique_ptr<VideoSource> source_;
    std::vector<VideoSource*> sinks_;
    bool running_ = false;
};

} // namespace video