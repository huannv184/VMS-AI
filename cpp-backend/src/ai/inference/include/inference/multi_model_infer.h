// inference/include/inference/multi_model_infer.h
#pragma once

#include "bounding_box.h"
#include "infer_config.h"
#include "advanced_infer.h"
#include <opencv2/opencv.hpp>
#include <memory>
#include <vector>
#include <string>
#include <cstring>

namespace inference {

// Forward declarations
class TrtEngine;

// Face landmark (5 points: left_eye, right_eye, nose, left_mouth, right_mouth)
struct FaceLandmark5 {
    float points[10];  // [x1, y1, x2, y2, x3, y3, x4, y4, x5, y5]
};

// Face detection with landmarks
struct FaceDetectionResult {
    float x1, y1, x2, y2;
    float confidence;
    float similarity;
    FaceLandmark5 landmarks;
    float embedding[512];  // ArcFace feature vector
    int person_id;         // Matched person ID (-1 if unknown)
    std::string name;      // Matched person name
    
    FaceDetectionResult() : confidence(0), similarity(0), person_id(-1) {
        std::memset(embedding, 0, sizeof(embedding));
    }
};

struct LicensePlateResult {
    float x1, y1, x2, y2;
    float confidence;
    std::string text;
};

// Combined detection result
struct MultiModelResult {
    std::vector<BBox> objects;      // YOLO detections
    std::vector<BBox> fire_objects; // Fire/Smoke detections
    std::vector<FaceDetectionResult> faces; // SCRFD + ArcFace
    std::vector<LicensePlateResult> plates; // LPR
    uint32_t frame_id;
    uint64_t timestamp_ms;
    
    // Processing times
    double yolo_time_ms;
    double face_detect_time_ms;
    double face_recog_time_ms;
    double lpr_time_ms;
    double total_time_ms;
};

// Multi-model inference manager
class MultiModelInfer {
public:
    struct Config {
        // YOLO model
        bool enable_yolo = true;
        std::string yolo_model_path;
        int yolo_input_size = 640;
        float yolo_conf_threshold = 0.25f;
        float yolo_nms_threshold = 0.45f;
        
        // SCRFD face detection model
        std::string scrfd_model_path;
        int scrfd_input_size = 640;
        float scrfd_conf_threshold = 0.5f;
        float scrfd_nms_threshold = 0.4f;
        
        // ArcFace recognition model
        std::string arcface_model_path;
        int arcface_input_size = 112;
        
        // Fire/Smoke Detection
        std::string fire_model_path;
        int fire_input_size = 640;
        float fire_conf_threshold = 0.25f;

        // LPR (License Plate Recognition)
        std::string plate_detect_model_path; // YOLOv8-plate
        std::string lpr_model_path;          // LPRNet
        int plate_detect_input_size = 640;
        
        // Feature matching (ArcFace cosine similarity)
        // 0.45 is too low (many false positives). 0.65-0.70 is standard for robust recognition.
        float face_match_threshold = 0.65f;
        
        // Performance
        bool enable_face_detection = true;
        bool enable_face_recognition = true;
        bool enable_fire_detection = false;
        bool enable_lpr = false;
        int max_faces_per_frame = 50;
    };

    explicit MultiModelInfer(const Config& config);
    ~MultiModelInfer();

    // Initialize all models
    bool init();
    
    // Run full inference on a frame (YOLO + Face + Fire + LPR)
    MultiModelResult infer(const cv::Mat& frame, uint32_t frame_id = 0);

    // Run everything EXCEPT YOLO — use when YOLO results come from BatchInferenceScheduler.
    // Caller provides pre-computed YOLO detections; this runs Face/Fire/LPR only.
    MultiModelResult inferNonYolo(const cv::Mat& frame, uint32_t frame_id,
                                  const std::vector<BBox>& yolo_objects);
    
    // Face database management
    bool addPerson(int id, const std::string& name, const std::vector<float>& embedding);
    bool removePerson(int person_id);
    void clearDatabase();
    
    // Feature Extraction (For Search)
    std::vector<float> embedImage(const cv::Mat& image);
    
    // Get metrics
    InferenceMetrics getMetrics() const;

private:
    Config config_;
    
    // Model instances
    std::unique_ptr<AdvancedInfer> yolo_infer_;
    std::unique_ptr<AdvancedInfer> fire_infer_;
    std::unique_ptr<AdvancedInfer> plate_detect_infer_;
    std::unique_ptr<TrtEngine> scrfd_engine_;
    std::unique_ptr<TrtEngine> arcface_engine_;
    std::unique_ptr<TrtEngine> lpr_engine_;
    
    // Face database
    struct PersonEntry {
        int id;
        std::string name;
        std::vector<float> embedding;
    };
    std::vector<PersonEntry> face_database_;
    int next_person_id_;
    
    // Metrics
    InferenceMetrics metrics_;
    
    // Helper functions
    std::vector<FaceDetectionResult> detectFaces(const cv::Mat& frame);
    void extractFaceEmbeddings(const cv::Mat& frame, std::vector<FaceDetectionResult>& faces);
    void matchFaces(std::vector<FaceDetectionResult>& faces);
    
    std::string decodeLPR(const std::vector<float>& output);
    void recognizePlates(const cv::Mat& frame, std::vector<LicensePlateResult>& plates);
    
    float cosineSimilarity(const float* a, const float* b, int dim);
    cv::Mat alignFace(const cv::Mat& frame, const FaceDetectionResult& face);
    
    std::vector<FaceDetectionResult> applyFaceNMS(const std::vector<FaceDetectionResult>& faces, float threshold);
    float calculateFaceIoU(const FaceDetectionResult& a, const FaceDetectionResult& b);
};

} // namespace inference