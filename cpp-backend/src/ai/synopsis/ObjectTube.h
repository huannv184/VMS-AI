#pragma once
#include <vector>
#include <opencv2/opencv.hpp>

namespace ai {
namespace synopsis {

/**
 * Represents a "tube" of a moving object across multiple frames.
 * A tube consists of the object's image crop, its spatial position,
 * and its temporal position (original timestamp).
 */
struct ObjectTube {
    int id; // Unique Tube ID
    int classId; // Setup from detection (e.g., Person, Car)
    std::string label;

    // Temporal info
    long long startTime; // Original start timestamp (ms)
    long long endTime;   // Original end timestamp (ms)
    
    // Scheduled info for synopsis
    long long scheduledStartTime; // When it will appear in the synopsis

    struct TubeFrame {
        long long timestamp; // Original timestamp
        cv::Rect rect;       // Bounding box in original frame
        cv::Mat crop;        // Image crop of the object
        cv::Mat mask;        // Binary mask (for better segmentation if available)
    };

    std::vector<TubeFrame> frames;

    // Helper: Get duration
    long long duration() const {
        return endTime - startTime;
    }
};

} // namespace synopsis
} // namespace ai
