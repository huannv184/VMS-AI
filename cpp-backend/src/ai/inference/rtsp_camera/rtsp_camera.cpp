#include "rtsp_camera.h"

RtspCamera::RtspCamera(const std::string& url)
{
    cap_.open(url);
}

bool RtspCamera::start()
{
    return cap_.isOpened();
}

bool RtspCamera::read(cv::Mat& frame)
{
    return cap_.read(frame);
}
