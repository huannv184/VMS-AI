// inference/src/advanced_infer.cpp
#include "inference/advanced_infer.h"
#include "inference/dummy.h"
#include "trt_engine.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace inference {

// COCO class names
static const char* COCO_CLASSES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
    "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
    "sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
    "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
    "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv",
    "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave",
    "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
    "scissors", "teddy bear", "hair drier", "toothbrush"
};

AdvancedInfer::AdvancedInfer(const InferConfig& config)
    : InferBase(config), config_(config)
{
    std::cout << "[AdvancedInfer] Created" << std::endl;
}

AdvancedInfer::~AdvancedInfer() = default;

bool AdvancedInfer::init() {
    std::cout << "\n[AdvancedInfer] ========================================" << std::endl;
    std::cout << "[AdvancedInfer] Initializing..." << std::endl;
    std::cout << "[AdvancedInfer] ========================================" << std::endl;
    
    if (config_.model_path.empty()) {
        std::cerr << "[AdvancedInfer] ERROR: No model path provided!" << std::endl;
        return false;
    }
    
    std::ifstream file(config_.model_path);
    if (!file.good()) {
        std::cerr << "[AdvancedInfer] ERROR: Model file not found: " 
                  << config_.model_path << std::endl;
        return false;
    }
    file.close();
    
    std::cout << "[AdvancedInfer] Loading YOLO engine: " << config_.model_path << std::endl;
    yolo_engine_ = std::make_unique<TrtEngine>(config_.model_path);
    
    if (!yolo_engine_->loadEngine()) {
        std::cerr << "[AdvancedInfer] Failed to load YOLO engine" << std::endl;
        yolo_engine_.reset();
        return false;
    }
    
    std::cout << "[AdvancedInfer] YOLO engine loaded successfully!" << std::endl;
    
    metrics_ = InferenceMetrics();
    std::cout << "[AdvancedInfer] Initialization complete!" << std::endl;
    return true;
}

// ============================================================================
// Generic Inference Interface
// ============================================================================

bool AdvancedInfer::infer(const InferInput& input, InferOutput& output) {
    auto start = std::chrono::high_resolution_clock::now();
    
    if (input.image.empty()) {
        std::cerr << "[AdvancedInfer] Empty input image" << std::endl;
        output.success = false;
        return false;
    }
    
    try {
        output.objects = detectObjects(input.image);
        output.faces.clear();
        output.success = true;
        
        auto end = std::chrono::high_resolution_clock::now();
        output.inference_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        
        updateMetrics(output.inference_time_ms, output.objects.size());
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[AdvancedInfer] Exception in infer: " << e.what() << std::endl;
        output.success = false;
        output.inference_time_ms = 0.0;
        return false;
    }
}

// ============================================================================
// OBJECT DETECTION
// ============================================================================

std::vector<BBox> AdvancedInfer::detectObjects(const cv::Mat& frame) {
    if (!yolo_engine_) return {};
    return detect(frame);
}

std::vector<BBox> AdvancedInfer::detect(const cv::Mat& image) {
    if (image.empty()) return {};
    
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<BBox> detections;
    
    if (yolo_engine_ && yolo_engine_->isLoaded()) {
        try {
            cv::Mat preprocessed = preprocessYOLO(image, config_.input_width, config_.input_height);
            std::vector<float> input_tensor = hwcToCHW(preprocessed);
            
            std::vector<float> output;
            if (!yolo_engine_->infer(input_tensor, output)) {
                std::cerr << "[AdvancedInfer] YOLO inference failed" << std::endl;
                return {};
            }
            
            detections = parseYOLOv8Output(output, config_.conf_threshold, config_.nms_threshold, image.cols, image.rows);
            // detections = applyNMS(detections, config_.nms_threshold); // NMS is now inside if implied by usage? No, keeping applyNMS call just in case or remove if it's double.
            // Wait, previous logic called applyNMS explicitly.
            detections = applyNMS(detections, config_.nms_threshold);
            
            detections.erase(
                std::remove_if(detections.begin(), detections.end(),
                    [this](const BBox& box) {
                        return box.score < config_.conf_threshold;
                    }),
                detections.end()
            );
        } catch (const std::exception& e) {
            std::cerr << "[AdvancedInfer] Exception in YOLO: " << e.what() << std::endl;
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    double inference_time = std::chrono::duration<double, std::milli>(end - start).count();
    
    updateMetrics(inference_time, detections.size());
    return detections;
}

cv::Mat AdvancedInfer::preprocessYOLO(const cv::Mat& image, int target_w, int target_h) {
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(target_w, target_h));
    
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    
    cv::Mat float_img;
    rgb.convertTo(float_img, CV_32FC3, 1.0 / 255.0);
    
    return float_img;
}

std::vector<float> AdvancedInfer::hwcToCHW(const cv::Mat& image) {
    const int height = image.rows;
    const int width = image.cols;
    const int channels = image.channels();
    
    std::vector<float> result(height * width * channels);
    
    const float* data = reinterpret_cast<const float*>(image.data);
    for (int c = 0; c < channels; ++c) {
        for (int h = 0; h < height; ++h) {
            for (int w = 0; w < width; ++w) {
                result[c * height * width + h * width + w] = 
                    data[(h * width + w) * channels + c];
            }
        }
    }
    
    return result;
}

std::vector<BBox> AdvancedInfer::parseYOLOv8Output(
    const std::vector<float>& output,
    float conf_thres,
    float nms_thres,
    int orig_width,
    int orig_height)
{
    std::vector<BBox> boxes;

    const int num_classes = 80;
    const int num_predictions = 8400;
    const int output_dims = 84;

    if (output.size() < num_predictions * output_dims) {
        std::cerr << "[AdvancedInfer] Invalid output size: "
                  << output.size() << std::endl;
        return boxes;
    }

    const float scale_x = static_cast<float>(orig_width) / config_.input_width;
    const float scale_y = static_cast<float>(orig_height) / config_.input_height;

    boxes.reserve(num_predictions / 10);

    for (int i = 0; i < num_predictions; ++i) {
        float cx = output[0 * num_predictions + i];
        float cy = output[1 * num_predictions + i];
        float w  = output[2 * num_predictions + i];
        float h  = output[3 * num_predictions + i];

        float best_conf = 0.0f;
        int best_class = -1;

        for (int c = 0; c < num_classes; ++c) {
            float conf = output[(4 + c) * num_predictions + i];
            if (conf > best_conf) {
                best_conf = conf;
                best_class = c;
            }
        }

        if (best_conf < conf_thres)
            continue;

        cx *= scale_x;
        cy *= scale_y;
        w  *= scale_x;
        h  *= scale_y;

        BBox box;
        box.x1 = cx - w * 0.5f;
        box.y1 = cy - h * 0.5f;
        box.x2 = cx + w * 0.5f;
        box.y2 = cy + h * 0.5f;
        box.score = best_conf;
        box.class_id = best_class;

        if (best_class >= 0 && best_class < 80) {
            box.label = COCO_CLASSES[best_class];
        } else {
            box.label = "unknown";
        }

        boxes.push_back(box);
    }

    return boxes;
}

float AdvancedInfer::computeIOU(const BBox& a, const BBox& b) {
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    
    const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    
    const float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    const float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    const float union_area = area_a + area_b - intersection;
    
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

std::vector<BBox> AdvancedInfer::applyNMS(
    const std::vector<BBox>& boxes,
    float threshold)
{
    if (boxes.empty()) return {};
    
    std::vector<BBox> result;
    std::vector<BBox> sorted_boxes = boxes;
    
    std::sort(sorted_boxes.begin(), sorted_boxes.end(),
        [](const BBox& a, const BBox& b) {
            return a.score > b.score;
        });
    
    std::vector<bool> suppressed(sorted_boxes.size(), false);
    
    for (size_t i = 0; i < sorted_boxes.size(); ++i) {
        if (suppressed[i]) continue;
        
        result.push_back(sorted_boxes[i]);
        
        const auto& box_a = sorted_boxes[i];
        
        for (size_t j = i + 1; j < sorted_boxes.size(); ++j) {
            if (suppressed[j]) continue;
            
            const auto& box_b = sorted_boxes[j];
            
            if (box_a.class_id != box_b.class_id) continue;
            
            const float iou = computeIOU(box_a, box_b);
            
            if (iou > threshold) {
                suppressed[j] = true;
            }
        }
    }
    
    return result;
}

// ============================================================================
// FACE DETECTION (SCRFD) - Stub implementations
// ============================================================================

std::vector<FaceDetection> AdvancedInfer::detectFaces(const cv::Mat& frame) {
    if (frame.empty() || !scrfd_engine_ || !scrfd_engine_->isLoaded()) {
        return {};
    }
    return {};
}

cv::Mat AdvancedInfer::preprocessSCRFD(const cv::Mat& image) {
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(640, 640));
    
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    
    cv::Mat float_img;
    rgb.convertTo(float_img, CV_32FC3, 1.0);
    float_img = (float_img - 127.5) / 128.0;
    
    return float_img;
}

std::vector<FaceDetection> AdvancedInfer::parseSCRFDOutput(
    const std::vector<std::vector<float>>& outputs,
    int orig_width, int orig_height)
{
    return {};
}

std::vector<FaceDetection> AdvancedInfer::nmsForFaces(
    const std::vector<FaceDetection>& faces,
    float threshold)
{
    return {};
}

// ============================================================================
// FACE RECOGNITION (ARCFACE)
// ============================================================================

std::vector<float> AdvancedInfer::extractFaceFeature(const cv::Mat& face_image) {
    return {};
}

cv::Mat AdvancedInfer::preprocessArcFace(const cv::Mat& face_image) {
    cv::Mat resized;
    cv::resize(face_image, resized, cv::Size(112, 112));
    
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    
    cv::Mat float_img;
    rgb.convertTo(float_img, CV_32FC3, 1.0);
    float_img = (float_img - 127.5) / 128.0;
    
    return float_img;
}

std::string AdvancedInfer::recognizeFace(const cv::Mat& face_image, float* similarity) {
    return "unknown";
}

cv::Mat AdvancedInfer::alignFace(const cv::Mat& image, const FaceDetection& face) {
    const int x1 = std::max(0, static_cast<int>(face.x1));
    const int y1 = std::max(0, static_cast<int>(face.y1));
    const int x2 = std::min(image.cols, static_cast<int>(face.x2));
    const int y2 = std::min(image.rows, static_cast<int>(face.y2));
    
    if (x2 > x1 && y2 > y1) {
        return image(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
    }
    
    return image.clone();
}

void AdvancedInfer::normalizeFeature(std::vector<float>& feature) {
    if (feature.empty()) return;
    
    float norm = 0.0f;
    for (const float val : feature) {
        norm += val * val;
    }
    norm = std::sqrt(norm);
    
    if (norm > 1e-6f) {
        const float inv_norm = 1.0f / norm;
        for (float& val : feature) {
            val *= inv_norm;
        }
    }
}

float AdvancedInfer::computeSimilarity(
    const std::vector<float>& feat1,
    const std::vector<float>& feat2)
{
    if (feat1.size() != feat2.size() || feat1.empty()) return 0.0f;
    
    float dot = 0.0f;
    for (size_t i = 0; i < feat1.size(); ++i) {
        dot += feat1[i] * feat2[i];
    }
    
    return dot;
}

// ============================================================================
// FACE DATABASE
// ============================================================================

bool AdvancedInfer::addFaceToDatabase(
    const std::string& name,
    const std::vector<float>& feature)
{
    if (feature.empty()) return false;
    
    FaceIdentity identity(name, feature, 1.0f);
    face_database_.push_back(identity);
    std::cout << "[AdvancedInfer] Added face to database: " << name << std::endl;
    return true;
}

bool AdvancedInfer::removeFaceFromDatabase(const std::string& name) {
    auto it = std::find_if(face_database_.begin(), face_database_.end(),
        [&name](const FaceIdentity& id) { return id.name == name; });
    
    if (it != face_database_.end()) {
        face_database_.erase(it);
        std::cout << "[AdvancedInfer] Removed face from database: " << name << std::endl;
        return true;
    }
    return false;
}

void AdvancedInfer::clearDatabase() {
    face_database_.clear();
    std::cout << "[AdvancedInfer] Cleared face database" << std::endl;
}

size_t AdvancedInfer::getDatabaseSize() const {
    return face_database_.size();
}

bool AdvancedInfer::loadDatabase(const std::string& filepath) {
    // Stub
    return true;
}

bool AdvancedInfer::saveDatabase(const std::string& filepath) const {
    // Stub
    return true;
}

// ============================================================================
// METRICS
// ============================================================================

// Redundant getMetrics removed (defined in header)

void AdvancedInfer::resetMetrics() {
    metrics_ = InferenceMetrics();
}

void AdvancedInfer::updateMetrics(double inference_time_ms, size_t num_detections) {
    metrics_.total_frames++;
    metrics_.total_detections += num_detections;
    
    const double alpha = 0.1;
    metrics_.avg_inference_ms = alpha * inference_time_ms + 
                                (1.0 - alpha) * metrics_.avg_inference_ms;
    
    if (metrics_.avg_inference_ms > 0.0) {
        metrics_.fps = 1000.0 / metrics_.avg_inference_ms;
    }
}

} // namespace inference