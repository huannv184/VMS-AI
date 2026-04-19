#pragma once
#include "pipeline.h"
#include <opencv2/opencv.hpp>
#include <thread>
#include <mutex>

namespace inference {

class MultiCamPipeline {
public:
    MultiCamPipeline(const std::vector<std::string>& src,
                     const std::string& engine);

    void run();

private:
    std::vector<cv::VideoCapture> caps_;
    std::vector<std::unique_ptr<Pipeline>> pipes_;
    std::mutex mtx_;

    void process(int idx);
};

}
