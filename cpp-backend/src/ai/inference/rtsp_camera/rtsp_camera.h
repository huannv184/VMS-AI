#pragma once
#include <opencv2/opencv.hpp>
#include <string>

class RtspCamera {
public:
    explicit RtspCamera(const std::string& url);

    bool start();
    bool read(cv::Mat& frame);

private:
    cv::VideoCapture cap_;
};
