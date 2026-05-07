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
#include <stdexcept>
#include <string>

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
            
            // parseYOLOv8Output does NOT run NMS — applyNMS is the single
            // place IoU suppression happens. Keep these as two distinct steps.
            detections = parseYOLOv8Output(output, config_.conf_threshold, config_.nms_threshold, image.cols, image.rows);
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
    // Letterbox: preserve aspect ratio + grey-pad to (target_w, target_h).
    // YOLOv8/YOLO11 were trained with letterboxed inputs; plain cv::resize
    // squashes 16:9 cameras into 1:1 → measurable accuracy regression
    // (smaller mAP, more false negatives) on people/cars.
    const float src_w = static_cast<float>(image.cols);
    const float src_h = static_cast<float>(image.rows);
    const float scale = std::min(target_w / src_w, target_h / src_h);

    const int new_w = std::max(1, static_cast<int>(std::round(src_w * scale)));
    const int new_h = std::max(1, static_cast<int>(std::round(src_h * scale)));
    const int pad_x = (target_w - new_w) / 2;
    const int pad_y = (target_h - new_h) / 2;

    yolo_letterbox_scale_ = scale;
    yolo_letterbox_pad_x_ = pad_x;
    yolo_letterbox_pad_y_ = pad_y;

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);

    // Grey 114 is the Ultralytics default fill colour
    cv::Mat canvas(target_h, target_w, image.type(), cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect(pad_x, pad_y, new_w, new_h)));

    cv::Mat rgb;
    cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);

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

    // DIAG: log output size + max confidence once every 200 calls so we can verify
    // the engine output matches the (1, 84, 8400) layout this parser assumes.
    static std::atomic<int> diag_counter{0};
    bool diag_now = (diag_counter.fetch_add(1) % 200 == 0);

    if (output.size() < (size_t)(num_predictions * output_dims)) {
        std::cerr << "[AdvancedInfer] Invalid output size: " << output.size()
                  << " (expected >= " << (num_predictions * output_dims) << ")" << std::endl;
        return boxes;
    }
    if (diag_now) {
        std::cerr << "[AdvancedInfer] YOLO output size=" << output.size()
                  << " (expected " << (num_predictions * output_dims) << ")"
                  << " conf_thres=" << conf_thres << std::endl;
    }

    // Unscale through letterbox: (model_coord - pad) / scale → source coord.
    // scale=0 fallback to old behaviour (e.g. inference path that bypassed preprocessYOLO).
    const float lb_scale = (yolo_letterbox_scale_ > 0.0f) ? yolo_letterbox_scale_ : 1.0f;
    const float lb_pad_x = static_cast<float>(yolo_letterbox_pad_x_);
    const float lb_pad_y = static_cast<float>(yolo_letterbox_pad_y_);

    boxes.reserve(num_predictions / 10);

    // YOLO output layout — Ultralytics exports YOLOv8/v11 to TensorRT in TWO
    // possible shapes depending on exporter version:
    //   - "channels-first" (1, 84, 8400) → output[c*8400 + i] = ch c of pred i
    //   - "channels-last"  (1, 8400, 84) → output[i*84 + c]   = ch c of pred i
    //
    // Auto-detect on first frame: try both layouts, the one producing higher
    // max class score is the correct one (the other layout reads cross-feature
    // values which are uncorrelated noise, max around 0.05-0.2).
    //
    // Override: VMS_YOLO_OUTPUT_LAYOUT=channels_last|channels_first (default auto)
    // Sigmoid: 2026-04-26 evidence shows our YOLO11 TRT engine emits POST-SIGMOID
    // probabilities (max in [0.05, 0.87], a real object scores ~0.6-0.9, empty
    // scene ~0.05). Re-applying sigmoid would push every anchor's max above 0.5,
    // producing kept=8400/8400 false positives. So default OFF.
    // Set VMS_YOLO_APPLY_SIGMOID=1 if a future engine ships raw logits instead.
    enum class Layout { Auto, ChannelsFirst, ChannelsLast };
    static const Layout layout_mode = []() {
        const char* env = std::getenv("VMS_YOLO_OUTPUT_LAYOUT");
        if (!env || !*env) return Layout::Auto;
        std::string s(env);
        if (s == "channels_last" || s == "last") return Layout::ChannelsLast;
        if (s == "channels_first" || s == "first") return Layout::ChannelsFirst;
        return Layout::Auto;
    }();
    static std::atomic<int> detected_layout{-1}; // -1=undecided, 0=first, 1=last

    static const bool apply_sigmoid = []() {
        const char* env = std::getenv("VMS_YOLO_APPLY_SIGMOID");
        if (!env || !*env) return false;  // engine output is already post-sigmoid
        return std::atoi(env) != 0;
    }();

    auto sigmoid = [](float v) { return 1.0f / (1.0f + std::exp(-v)); };

    // Auto-detect layout once: probe first prediction in both layouts and pick
    // whichever produces a sane max class score (>= 0.3 raw). Falls through to
    // ChannelsFirst if neither is clearly better.
    int decided_layout = detected_layout.load(std::memory_order_relaxed);
    if (decided_layout == -1) {
        if (layout_mode == Layout::ChannelsFirst) {
            decided_layout = 0;
        } else if (layout_mode == Layout::ChannelsLast) {
            decided_layout = 1;
        } else {
            // Probe both layouts. A correct layout reads class scores which
            // must lie in either [0, 1] (sigmoid'd) or roughly [-10, 10]
            // (raw logits). A WRONG layout reads coordinate channels which
            // contain pixel values up to ~640 — instantly disqualifying.
            //
            // So: pick the layout whose max class score is in the SANE range.
            // If both are sane, pick the higher one. If neither is sane,
            // default to channels_first (Ultralytics' standard export).
            float max_first = 0.0f, max_last = 0.0f;
            for (int i = 0; i < num_predictions; ++i) {
                for (int c = 0; c < num_classes; ++c) {
                    float v_first = output[(4 + c) * num_predictions + i];
                    float v_last  = output[i * output_dims + (4 + c)];
                    if (v_first > max_first) max_first = v_first;
                    if (v_last  > max_last)  max_last  = v_last;
                }
            }
            const float SANE_MAX = 20.0f; // sigmoid'd <=1, logits typically <=10
            const bool first_sane = max_first <= SANE_MAX;
            const bool last_sane  = max_last  <= SANE_MAX;
            const char* reason = "default";
            if (first_sane && !last_sane) {
                decided_layout = 0; reason = "channels_last reads coords (>20)";
            } else if (last_sane && !first_sane) {
                decided_layout = 1; reason = "channels_first reads coords (>20)";
            } else if (first_sane && last_sane) {
                decided_layout = (max_last > max_first) ? 1 : 0;
                reason = "both sane, picked larger";
            } else {
                decided_layout = 0; reason = "neither sane, default channels_first";
            }
            std::cerr << "[AdvancedInfer] YOLO layout autodetect: "
                      << "max_first=" << max_first << " max_last=" << max_last
                      << " → using " << (decided_layout == 1 ? "channels_last" : "channels_first")
                      << " (" << reason << ")" << std::endl;
        }
        detected_layout.store(decided_layout, std::memory_order_relaxed);
    }
    const bool channels_last = (decided_layout == 1);

    float global_max_conf_raw = 0.0f;
    int global_max_cls = -1;
    float global_max_conf = 0.0f; // post-sigmoid value used for decisions

    auto at = [&](int i, int c) -> float {
        return channels_last ? output[i * output_dims + c]
                              : output[c * num_predictions + i];
    };

    for (int i = 0; i < num_predictions; ++i) {
        float cx = at(i, 0);
        float cy = at(i, 1);
        float w  = at(i, 2);
        float h  = at(i, 3);

        float best_conf_raw = 0.0f;
        int best_class = -1;

        for (int c = 0; c < num_classes; ++c) {
            float conf = at(i, 4 + c);
            if (conf > best_conf_raw) {
                best_conf_raw = conf;
                best_class = c;
            }
        }

        if (best_conf_raw > global_max_conf_raw) {
            global_max_conf_raw = best_conf_raw;
            global_max_cls = best_class;
        }

        float best_conf = apply_sigmoid ? sigmoid(best_conf_raw) : best_conf_raw;
        if (best_conf > global_max_conf) global_max_conf = best_conf;

        if (best_conf < conf_thres)
            continue;

        // Strip letterbox padding then divide by uniform scale.
        cx = (cx - lb_pad_x) / lb_scale;
        cy = (cy - lb_pad_y) / lb_scale;
        w  /= lb_scale;
        h  /= lb_scale;

        BBox box;
        box.x1 = std::max(0.0f, cx - w * 0.5f);
        box.y1 = std::max(0.0f, cy - h * 0.5f);
        box.x2 = std::min(static_cast<float>(orig_width),  cx + w * 0.5f);
        box.y2 = std::min(static_cast<float>(orig_height), cy + h * 0.5f);
        box.score = best_conf;
        box.class_id = best_class;

        if (best_class >= 0 && best_class < 80) {
            box.label = COCO_CLASSES[best_class];
        } else {
            box.label = "unknown";
        }

        boxes.push_back(box);
    }

    if (diag_now) {
        std::cerr << "[AdvancedInfer] YOLO layout=" << (channels_last ? "last" : "first")
                  << " sigmoid=" << (apply_sigmoid ? "on" : "off")
                  << " max_conf_raw=" << global_max_conf_raw
                  << " max_conf=" << global_max_conf
                  << " (cls=" << global_max_cls << ")"
                  << " kept=" << boxes.size() << "/" << num_predictions << std::endl;
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
// FACE DETECTION (SCRFD) — UNIMPLEMENTED on AdvancedInfer.
//
// Production face detection runs through `MultiModelInfer::detectFaces` (real
// SCRFD parsing + post-processing in `multi_model_infer.cpp`). The methods
// below were never wired and were previously `return {};` stubs — that's an
// operational lie if a new caller ever picks them up: the empty result looks
// indistinguishable from "no faces in frame". They now throw so any future
// instantiation fails loud at first call.
// ============================================================================

[[noreturn]] static void faceMethodNotImplemented(const char* fn) {
    throw std::logic_error(
        std::string("AdvancedInfer::") + fn +
        " is not implemented. Use MultiModelInfer for production face inference; "
        "AdvancedInfer is for object/fire/plate detection only.");
}

std::vector<FaceDetection> AdvancedInfer::detectFaces(const cv::Mat& /*frame*/) {
    faceMethodNotImplemented("detectFaces");
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
    const std::vector<std::vector<float>>& /*outputs*/,
    int /*orig_width*/, int /*orig_height*/)
{
    faceMethodNotImplemented("parseSCRFDOutput");
}

std::vector<FaceDetection> AdvancedInfer::nmsForFaces(
    const std::vector<FaceDetection>& /*faces*/,
    float /*threshold*/)
{
    faceMethodNotImplemented("nmsForFaces");
}

// ============================================================================
// FACE RECOGNITION (ARCFACE)
// ============================================================================

std::vector<float> AdvancedInfer::extractFaceFeature(const cv::Mat& /*face_image*/) {
    faceMethodNotImplemented("extractFaceFeature");
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

std::string AdvancedInfer::recognizeFace(const cv::Mat& /*face_image*/, float* /*similarity*/) {
    faceMethodNotImplemented("recognizeFace");
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

bool AdvancedInfer::loadDatabase(const std::string& /*filepath*/) {
    // Database persistence not implemented on AdvancedInfer. Returning
    // `false` (rather than throwing) so callers see a clean "could not load"
    // signal — boot paths often try this opportunistically and we don't want
    // to abort startup just because no file was provided. Pair with
    // `saveDatabase` below.
    return false;
}

bool AdvancedInfer::saveDatabase(const std::string& /*filepath*/) const {
    return false;
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