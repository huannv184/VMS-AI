// inference/include/inference/advanced_infer.h
#pragma once

#include "infer_config.h"
#include "infer_base.h"
#include "dummy.h"           // ← Include TRƯỚC để có FaceDetection
#include "bounding_box.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>

namespace inference {

// Forward declaration
class TrtEngine;

// ============================================================================
// ❌ XÓA TẤT CẢ STRUCT FaceLandmark và FaceDetection Ở ĐÂY
// (Vì đã có trong dummy.h rồi!)
// ============================================================================

// ============================================================================
// Face Identity Structure
// ============================================================================

struct FaceIdentity {
    std::string name;
    std::vector<float> feature;
    float confidence;
    
    FaceIdentity() : confidence(0.0f) {}
    FaceIdentity(const std::string& n, const std::vector<float>& f, float c)
        : name(n), feature(f), confidence(c) {}
};

// ============================================================================
// Inference Metrics
// ============================================================================

struct InferenceMetrics {
    uint64_t total_frames{0};
    uint64_t processed_frames{0};
    uint64_t failed_frames{0};
    double avg_inference_ms{0.0};
    double fps{0.0};
    uint64_t total_detections{0};
    uint64_t total_faces{0};
};

// ============================================================================
// Advanced Inference Class
// ============================================================================

class AdvancedInfer : public InferBase {
public:
    explicit AdvancedInfer(const InferConfig& config);
    ~AdvancedInfer() override;
    
    bool init() override;
    bool infer(const InferInput& input, InferOutput& output) override;
    
    // Object Detection (YOLO)
    std::vector<BBox> detectObjects(const cv::Mat& frame);
    std::vector<BBox> detect(const cv::Mat& image);
    
    // Face Detection (SCRFD)
    std::vector<FaceDetection> detectFaces(const cv::Mat& frame);
    
    // Face Recognition (ArcFace)
    std::vector<float> extractFaceFeature(const cv::Mat& face_image);
    std::string recognizeFace(const cv::Mat& face_image, float* similarity = nullptr);
    
    // Face Database Management
    bool addFaceToDatabase(const std::string& name, const std::vector<float>& feature);
    bool removeFaceFromDatabase(const std::string& name);
    void clearDatabase();
    size_t getDatabaseSize() const;
    bool loadDatabase(const std::string& filepath);
    bool saveDatabase(const std::string& filepath) const;
    
    // Metrics
    InferenceMetrics getMetrics() const { return metrics_; }
    void resetMetrics();

private:
    // YOLO specific helpers
    cv::Mat preprocessYOLO(const cv::Mat& image, int target_w, int target_h);
    std::vector<float> hwcToCHW(const cv::Mat& image);
    std::vector<BBox> parseYOLOv8Output(
        const std::vector<float>& output, 
        float conf_thres, 
        float nms_thres, 
        int width, 
        int height
    );
    std::vector<BBox> applyNMS(
        const std::vector<BBox>& boxes, 
        float nms_threshold
    );
    float computeIOU(const BBox& a, const BBox& b);
    
    // SCRFD helpers
    cv::Mat preprocessSCRFD(const cv::Mat& image);
    std::vector<FaceDetection> parseSCRFDOutput(
        const std::vector<std::vector<float>>& outputs,
        int orig_width, int orig_height
    );
    std::vector<FaceDetection> nmsForFaces(
        const std::vector<FaceDetection>& faces,
        float threshold
    );
    
    // ArcFace helpers
    cv::Mat preprocessArcFace(const cv::Mat& face_image);
    void normalizeFeature(std::vector<float>& feature);
    float computeSimilarity(const std::vector<float>& feat1, const std::vector<float>& feat2);
    cv::Mat alignFace(const cv::Mat& image, const FaceDetection& face);
    
    void updateMetrics(double inference_time_ms, size_t num_detections);
    
    InferConfig config_;
    InferenceMetrics metrics_;
    
    // TensorRT Engines
    std::unique_ptr<TrtEngine> yolo_engine_;
    std::unique_ptr<TrtEngine> scrfd_engine_;
    std::unique_ptr<TrtEngine> arcface_engine_;
    
    // Face Recognition
    std::vector<FaceIdentity> face_database_;
    float face_similarity_threshold_{0.5f};
    bool enable_face_detection_{false};
    bool enable_face_recognition_{false};

    // YOLO letterbox state — populated by preprocessYOLO, consumed by parseYOLOv8Output.
    // Letterbox preserves aspect ratio (no squashing) which is what YOLO11 was trained
    // with. Storing scale/pad here avoids changing the parse signature.
    float yolo_letterbox_scale_{1.0f};
    int   yolo_letterbox_pad_x_{0};
    int   yolo_letterbox_pad_y_{0};

    mutable std::mutex mutex_;
};

} // namespace inference