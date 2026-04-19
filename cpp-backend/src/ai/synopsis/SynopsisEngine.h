#pragma once
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>
#include "TubeManager.h"
#include <functional>

namespace ai {
namespace synopsis {

struct SynopsisConfig {
    std::string inputVideoPath;
    std::string outputVideoPath;
    int targetDurationSec = 60; // Desired summary length
    std::string bgModelPath; // Optional background model
};

class SynopsisEngine {
public:
    SynopsisEngine();
    ~SynopsisEngine();

    // Main function to run synopsis generation
    // progressCallback: function(float percent)
    bool generate(const SynopsisConfig& config, std::function<void(float)> progressCallback = nullptr);

private:
    cv::Mat generateBackground(const std::string& videoPath);
    void scheduleTubes(std::vector<ObjectTube>& tubes, int targetDurationMs);
    void renderSynopsis(const std::string& outputPath, const cv::Mat& background, 
                        const std::vector<ObjectTube>& tubes, int width, int height, double fps, int targetDurationMs);
};

} // namespace synopsis
} // namespace ai
