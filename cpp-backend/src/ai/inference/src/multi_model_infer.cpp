// inference/src/multi_model_infer.cpp - PRODUCTION VERSION (NO DEBUG)
#include "inference/multi_model_infer.h"
#include "inference/advanced_infer.h"
#include "trt_engine.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>

namespace inference {

MultiModelInfer::MultiModelInfer(const Config& config)
    : config_(config), next_person_id_(0) {
    std::cerr << "[MultiModelInfer] Created" << std::endl;
}

MultiModelInfer::~MultiModelInfer() {
    std::cerr << "[MultiModelInfer] Destroyed" << std::endl;
}

bool MultiModelInfer::init() {
    std::cerr << "\n========================================" << std::endl;
    std::cerr << "[MultiModelInfer] Initializing Multi-Model System..." << std::endl;
    std::cerr << "========================================\n" << std::endl;
    
    // YOLO INITIALIZATION
    if (config_.enable_yolo) {
        if (config_.yolo_model_path.empty()) {
            std::cerr << "[MultiModelInfer] ERROR: YOLO model path is empty!" << std::endl;
            // config_.enable_yolo = false; // Don't disable here, return false as it is crucial? Or just warn?
            // Let's warn and disable
            config_.enable_yolo = false;
        } else {
            std::ifstream yolo_file(config_.yolo_model_path);
            if (!yolo_file.good()) {
                std::cerr << "[MultiModelInfer] ERROR: YOLO model not found: " << config_.yolo_model_path << std::endl;
                config_.enable_yolo = false;
            } else {
                yolo_file.close();
                
                std::cerr << "[MultiModelInfer] Loading YOLO model..." << std::endl;
                
                InferConfig yolo_cfg;
                yolo_cfg.model_path = config_.yolo_model_path;
                yolo_cfg.input_width = config_.yolo_input_size;
                yolo_cfg.input_height = config_.yolo_input_size;
                yolo_cfg.conf_threshold = config_.yolo_conf_threshold;
                yolo_cfg.nms_threshold = config_.yolo_nms_threshold;
                
                yolo_infer_ = std::make_unique<AdvancedInfer>(yolo_cfg);
                if (!yolo_infer_->init()) {
                    std::cerr << "[MultiModelInfer] CRITICAL: YOLO initialization failed!" << std::endl;
                    config_.enable_yolo = false;
                    yolo_infer_.reset();
                } else {
                    std::cerr << "[MultiModelInfer] ✓ YOLO loaded" << std::endl;
                }
            }
        }
    }

    // FIRE/SMOKE INITIALIZATION
    if (config_.enable_fire_detection && !config_.fire_model_path.empty()) {
        std::cerr << "[MultiModelInfer] Loading Fire/Smoke model..." << std::endl;
        std::ifstream fire_file(config_.fire_model_path);
        if (!fire_file.good()) {
            std::cerr << "[MultiModelInfer] WARNING: Fire model not found: " << config_.fire_model_path << std::endl;
            config_.enable_fire_detection = false;
        } else {
            fire_file.close();
            InferConfig fire_cfg;
            fire_cfg.model_path = config_.fire_model_path;
            fire_cfg.input_width = config_.fire_input_size;
            fire_cfg.input_height = config_.fire_input_size;
            fire_cfg.conf_threshold = config_.fire_conf_threshold;
            fire_cfg.nms_threshold = config_.yolo_nms_threshold; // Reuse same NMS for now

            fire_infer_ = std::make_unique<AdvancedInfer>(fire_cfg);
            if (!fire_infer_->init()) {
                std::cerr << "[MultiModelInfer] Fire model init failed - disabling" << std::endl;
                config_.enable_fire_detection = false;
                fire_infer_.reset();
            } else {
                 std::cerr << "[MultiModelInfer] ✓ Fire/Smoke model loaded" << std::endl;
            }
        }
    } else {
        config_.enable_fire_detection = false;
    }
    
    // SCRFD INITIALIZATION
    if (config_.enable_face_detection && !config_.scrfd_model_path.empty()) {
        std::cerr << "[MultiModelInfer] Loading SCRFD..." << std::endl;
        
        std::ifstream scrfd_file(config_.scrfd_model_path);
        if (!scrfd_file.good()) {
            std::cerr << "[MultiModelInfer] WARNING: SCRFD not found, face detection disabled" << std::endl;
            config_.enable_face_detection = false;
        } else {
            scrfd_file.close();
            scrfd_engine_ = std::make_unique<TrtEngine>(config_.scrfd_model_path);
            
            if (!scrfd_engine_->loadEngine()) {
                std::cerr << "[MultiModelInfer] WARNING: Failed to load SCRFD" << std::endl;
                config_.enable_face_detection = false;
                scrfd_engine_.reset();
            } else {
                std::cerr << "[MultiModelInfer] ✓ SCRFD loaded" << std::endl;
            }
        }
    } else {
        config_.enable_face_detection = false;
    }
    
    // LPR INITIALIZATION
    if (config_.enable_lpr && !config_.plate_detect_model_path.empty() && !config_.lpr_model_path.empty()) {
         std::cerr << "[MultiModelInfer] Loading LPR Models..." << std::endl;
         
         // 1. Plate Detector (YOLO)
         InferConfig plate_cfg;
         plate_cfg.model_path = config_.plate_detect_model_path;
         plate_cfg.input_width = config_.plate_detect_input_size;
         plate_cfg.input_height = config_.plate_detect_input_size;
         plate_cfg.conf_threshold = 0.4f; // High confidence for plates
         plate_cfg.nms_threshold = 0.45f;
         
         plate_detect_infer_ = std::make_unique<AdvancedInfer>(plate_cfg);
         if (!plate_detect_infer_->init()) {
             std::cerr << "[MultiModelInfer] Plate Detector init failed" << std::endl;
             config_.enable_lpr = false;
         } else {
             std::cerr << "[MultiModelInfer] ✓ Plate Detector loaded" << std::endl;
             
             // 2. LPRNet
             lpr_engine_ = std::make_unique<TrtEngine>(config_.lpr_model_path);
             if (!lpr_engine_->loadEngine()) {
                  std::cerr << "[MultiModelInfer] LPRNet init failed" << std::endl;
                  config_.enable_lpr = false;
                  lpr_engine_.reset();
             } else {
                  std::cerr << "[MultiModelInfer] ✓ LPRNet loaded" << std::endl;
             }
         }
    } else {
        config_.enable_lpr = false;
    }

    // ARCFACE INITIALIZATION
    if (config_.enable_face_recognition && !config_.arcface_model_path.empty()) {
        if (!config_.enable_face_detection) {
            std::cerr << "[MultiModelInfer] Face recognition disabled (no face detection)" << std::endl;
            config_.enable_face_recognition = false;
        } else {
            std::cerr << "[MultiModelInfer] Loading ArcFace..." << std::endl;
            
            std::ifstream arcface_file(config_.arcface_model_path);
            if (!arcface_file.good()) {
                std::cerr << "[MultiModelInfer] WARNING: ArcFace not found" << std::endl;
                config_.enable_face_recognition = false;
            } else {
                arcface_file.close();
                arcface_engine_ = std::make_unique<TrtEngine>(config_.arcface_model_path);
                
                if (!arcface_engine_->loadEngine()) {
                    std::cerr << "[MultiModelInfer] WARNING: Failed to load ArcFace" << std::endl;
                    config_.enable_face_recognition = false;
                    arcface_engine_.reset();
                } else {
                    std::cerr << "[MultiModelInfer] ✓ ArcFace loaded" << std::endl;
                }
            }
        }
    } else {
        config_.enable_face_recognition = false;
    }
    
    metrics_ = {};
    
    std::cerr << "\n========================================" << std::endl;
    std::cerr << "[MultiModelInfer] Initialization Complete!" << std::endl;
    std::cerr << "  " << (config_.enable_yolo ? "✓" : "✗") << " YOLO: " 
              << (config_.enable_yolo ? "ENABLED" : "DISABLED") << std::endl;
    std::cerr << "  " << (config_.enable_fire_detection ? "✓" : "✗") << " Fire/Smoke: " 
              << (config_.enable_fire_detection ? "ENABLED" : "DISABLED") << std::endl;
    std::cerr << "  " << (config_.enable_face_detection ? "✓" : "✗") << " SCRFD: " 
              << (config_.enable_face_detection ? "ENABLED" : "DISABLED") << std::endl;
    std::cerr << "  " << (config_.enable_face_recognition ? "✓" : "✗") << " ArcFace: " 
              << (config_.enable_face_recognition ? "ENABLED" : "DISABLED") << std::endl;
    std::cerr << "========================================\n" << std::endl;
    
    return true;
}

MultiModelResult MultiModelInfer::infer(const cv::Mat& frame, uint32_t frame_id) {
    auto start_total = std::chrono::high_resolution_clock::now();
    
    MultiModelResult result;
    result.frame_id = frame_id;
    result.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    result.yolo_time_ms = 0.0;
    result.face_detect_time_ms = 0.0;
    result.face_recog_time_ms = 0.0;
    
    // YOLO
    if (config_.enable_yolo && yolo_infer_) {
        try {
            auto start = std::chrono::high_resolution_clock::now();
            result.objects = yolo_infer_->detectObjects(frame);
            auto end = std::chrono::high_resolution_clock::now();
            result.yolo_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        } catch (const std::exception& e) {
            std::cerr << "[MultiModelInfer] YOLO error: " << e.what() << std::endl;
        }
    }

    // FIRE / SMOKE
    if (config_.enable_fire_detection && fire_infer_) {
        try {
            // We reuse yolo_time_ms accumulator or add a new one? 
            // For now, let's include it in total time but not break struct
            // Ideally we should add fire_time_ms to struct, but keeping it simple for now.
            result.fire_objects = fire_infer_->detectObjects(frame);
        } catch (const std::exception& e) {
             std::cerr << "[MultiModelInfer] Fire Infer error: " << e.what() << std::endl;
        }
    }
    
    // SCRFD
    if (config_.enable_face_detection && scrfd_engine_ && scrfd_engine_->isLoaded()) {
        try {
            auto start = std::chrono::high_resolution_clock::now();
            result.faces = detectFaces(frame);
            auto end = std::chrono::high_resolution_clock::now();
            result.face_detect_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        } catch (const std::exception& e) {
            std::cerr << "[MultiModelInfer] SCRFD error: " << e.what() << std::endl;
        }
        
        // ARCFACE (Nested under face detection)
        if (config_.enable_face_recognition && arcface_engine_ && 
            arcface_engine_->isLoaded() && !result.faces.empty()) {
            try {
                auto start = std::chrono::high_resolution_clock::now();
                extractFaceEmbeddings(frame, result.faces);
                matchFaces(result.faces);
                auto end = std::chrono::high_resolution_clock::now();
                result.face_recog_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
            } catch (const std::exception& e) {
                std::cerr << "[MultiModelInfer] ArcFace error: " << e.what() << std::endl;
            }
        }
    }
    
    // LPR
    if (config_.enable_lpr && plate_detect_infer_ && lpr_engine_) {
        try {
             auto start = std::chrono::high_resolution_clock::now();
             recognizePlates(frame, result.plates);
             auto end = std::chrono::high_resolution_clock::now();
             result.lpr_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        } catch (const std::exception& e) {
             std::cerr << "[MultiModelInfer] LPR error: " << e.what() << std::endl;
        }
    }
    
    auto end_total = std::chrono::high_resolution_clock::now();
    result.total_time_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();
    
    // Update metrics
    metrics_.total_frames++;
    double alpha = 0.1;
    metrics_.avg_inference_ms = alpha * result.total_time_ms + (1.0 - alpha) * metrics_.avg_inference_ms;
    if (metrics_.avg_inference_ms > 0) {
        metrics_.fps = 1000.0 / metrics_.avg_inference_ms;
    }
    
    return result;
}

// ============================================================================
// NON-YOLO INFERENCE — for use with BatchInferenceScheduler
// ============================================================================

MultiModelResult MultiModelInfer::inferNonYolo(
    const cv::Mat& frame, uint32_t frame_id,
    const std::vector<BBox>& yolo_objects)
{
    auto start_total = std::chrono::high_resolution_clock::now();

    MultiModelResult result;
    result.frame_id = frame_id;
    result.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    result.yolo_time_ms = 0.0;
    result.face_detect_time_ms = 0.0;
    result.face_recog_time_ms = 0.0;
    result.lpr_time_ms = 0.0;

    // YOLO results provided by batch scheduler
    result.objects = yolo_objects;

    // FIRE / SMOKE
    if (config_.enable_fire_detection && fire_infer_) {
        try {
            result.fire_objects = fire_infer_->detectObjects(frame);
        } catch (const std::exception& e) {
            std::cerr << "[MultiModelInfer] Fire Infer error: " << e.what() << std::endl;
        }
    }

    // SCRFD
    if (config_.enable_face_detection && scrfd_engine_ && scrfd_engine_->isLoaded()) {
        try {
            auto start = std::chrono::high_resolution_clock::now();
            result.faces = detectFaces(frame);
            auto end = std::chrono::high_resolution_clock::now();
            result.face_detect_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        } catch (const std::exception& e) {
            std::cerr << "[MultiModelInfer] SCRFD error: " << e.what() << std::endl;
        }

        // ARCFACE
        if (config_.enable_face_recognition && arcface_engine_ &&
            arcface_engine_->isLoaded() && !result.faces.empty()) {
            try {
                auto start = std::chrono::high_resolution_clock::now();
                extractFaceEmbeddings(frame, result.faces);
                matchFaces(result.faces);
                auto end = std::chrono::high_resolution_clock::now();
                result.face_recog_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
            } catch (const std::exception& e) {
                std::cerr << "[MultiModelInfer] ArcFace error: " << e.what() << std::endl;
            }
        }
    }

    // LPR
    if (config_.enable_lpr && plate_detect_infer_ && lpr_engine_) {
        try {
            auto start = std::chrono::high_resolution_clock::now();
            recognizePlates(frame, result.plates);
            auto end = std::chrono::high_resolution_clock::now();
            result.lpr_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        } catch (const std::exception& e) {
            std::cerr << "[MultiModelInfer] LPR error: " << e.what() << std::endl;
        }
    }

    auto end_total = std::chrono::high_resolution_clock::now();
    result.total_time_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();

    metrics_.total_frames++;
    double alpha = 0.1;
    metrics_.avg_inference_ms = alpha * result.total_time_ms + (1.0 - alpha) * metrics_.avg_inference_ms;
    if (metrics_.avg_inference_ms > 0) {
        metrics_.fps = 1000.0 / metrics_.avg_inference_ms;
    }

    return result;
}

// ============================================================================
// SCRFD FACE DETECTION - OPTIMIZED & CLEAN
// ============================================================================

std::vector<FaceDetectionResult> MultiModelInfer::detectFaces(const cv::Mat& frame) {
    std::vector<FaceDetectionResult> faces;
    
    if (!scrfd_engine_ || !scrfd_engine_->isLoaded() || frame.empty()) {
        return faces;
    }
    
    try {
        // Preprocess — InsightFace SCRFD spec (matches scrfd/tools/scrfd.py):
        //   blobFromImage(img, scale=1/128, size=(640,640), mean=(127.5,127.5,127.5), swapRB=True)
        //   • Swap BGR → RGB
        //   • (pixel - 127.5) / 128 → range ~[-1, 1]
        //   • LETTERBOX (preserve aspect ratio) — quan trọng vì frame 640×360 (16:9)
        //     resize thẳng lên 640×640 sẽ stretch theo trục dọc 1.78x → mặt người
        //     bị méo elongated → SCRFD trained trên ảnh aspect-correct → score sụt
        //     từ ~0.5 xuống ~0.15. Letterbox giữ tỷ lệ + pad đen.
        const int net_size = config_.scrfd_input_size;
        const float scale_letterbox = std::min((float)net_size / frame.cols,
                                               (float)net_size / frame.rows);
        const int new_w = static_cast<int>(std::round(frame.cols * scale_letterbox));
        const int new_h = static_cast<int>(std::round(frame.rows * scale_letterbox));
        const int pad_x = (net_size - new_w) / 2;
        const int pad_y = (net_size - new_h) / 2;

        cv::Mat resized;
        cv::resize(frame, resized, cv::Size(new_w, new_h));

        // Pad về 640×640 với value=0 (sau normalize sẽ ~ -1.0 vùng pad)
        cv::Mat letterboxed = cv::Mat::zeros(net_size, net_size, frame.type());
        resized.copyTo(letterboxed(cv::Rect(pad_x, pad_y, new_w, new_h)));

        cv::Mat rgb;
        cv::cvtColor(letterboxed, rgb, cv::COLOR_BGR2RGB);
        rgb.convertTo(rgb, CV_32F, 1.0 / 128.0, -127.5 / 128.0);

        // Convert to CHW
        std::vector<float> input_tensor(1 * 3 * config_.scrfd_input_size * config_.scrfd_input_size);
        std::vector<cv::Mat> channels(3);
        cv::split(rgb, channels);
        
        int channel_size = config_.scrfd_input_size * config_.scrfd_input_size;
        for (int c = 0; c < 3; c++) {
            std::memcpy(input_tensor.data() + c * channel_size,
                       channels[c].data, channel_size * sizeof(float));
        }
        
        // Inference
        std::vector<std::vector<float>> inputs = {input_tensor};
        std::vector<std::vector<float>> outputs;
        
        if (!scrfd_engine_->inferMulti(inputs, outputs)) {
            std::cerr << "[SCRFD] inferMulti FAILED" << std::endl;
            return faces;
        }
        if (outputs.size() != 9) {
            std::cerr << "[SCRFD] Expected 9 outputs, got " << outputs.size() << std::endl;
            return faces;
        }
        
        // Parse outputs
        const std::vector<int> strides = {8, 16, 32};
        const int input_size = config_.scrfd_input_size;

        // Letterbox unscale: bbox trong tọa độ 640×640 padded → bỏ padding rồi
        // chia scale_letterbox để về tọa độ frame gốc.
        const float inv_scale = 1.0f / scale_letterbox;
        float detection_threshold = config_.scrfd_conf_threshold;
        
        // Debug: Log output sizes
        static int scrfd_debug_count = 0;
        bool do_debug = (scrfd_debug_count++ % 500 == 0);
        if (do_debug) {
            std::cerr << "[SCRFD] Input: " << frame.cols << "x" << frame.rows 
                      << " -> " << input_size << "x" << input_size << std::endl;
            for (size_t i = 0; i < outputs.size(); i++) {
                std::cerr << "[SCRFD]   output[" << i << "].size=" << outputs[i].size() << std::endl;
            }
        }
        
        for (int scale_idx = 0; scale_idx < 3; ++scale_idx) {
            const auto& scores = outputs[scale_idx];
            const auto& bboxes = outputs[scale_idx + 3];
            const auto& landmarks = outputs[scale_idx + 6];
            
            int stride = strides[scale_idx];
            int feat_h = input_size / stride;
            int feat_w = input_size / stride;
            int num_anchors = feat_h * feat_w;
            
            // SCRFD 2.5g_bnkps has 2 anchors per location
            int num_anchors_per_loc = 2;
            int total_anchors = num_anchors * num_anchors_per_loc;
            
            int score_channels = (total_anchors > 0) ? scores.size() / total_anchors : 0;
            int bbox_channels = (total_anchors > 0) ? bboxes.size() / total_anchors : 0;
            int landmark_channels = (total_anchors > 0) ? landmarks.size() / total_anchors : 0;
            
            if (do_debug) {
                std::cerr << "[SCRFD] stride=" << stride << " feat=" << feat_h << "x" << feat_w 
                          << " anchors=" << total_anchors 
                          << " score_ch=" << score_channels << " bbox_ch=" << bbox_channels 
                          << " lm_ch=" << landmark_channels << std::endl;
                // Find max score
                float max_score = -1;
                for (size_t si = 0; si < scores.size(); si++) {
                    if (scores[si] > max_score) max_score = scores[si];
                }
                std::cerr << "[SCRFD] max_score=" << max_score << " (threshold=" << detection_threshold << ")" << std::endl;
            }
            
            if (score_channels != 1 && score_channels != 2) continue;
            if (bbox_channels != 4 && bbox_channels != 8) continue;
            
            for (int i = 0; i < total_anchors; ++i) {
                float score = (score_channels == 2) ? scores[i * 2 + 1] : scores[i];
                if (score < detection_threshold) continue;
                
                int loc_idx = i / num_anchors_per_loc;
                int anchor_y = loc_idx / feat_w;
                int anchor_x = loc_idx % feat_w;
                
                float cx = (anchor_x + 0.5f) * stride;
                float cy = (anchor_y + 0.5f) * stride;
                
                // Decode bbox: channels==8 means DFL regression (reg_max=1 → 2 bins per side)
                // Layout per anchor: [l0,l1, t0,t1, r0,r1, b0,b1]
                // Expected distance = softmax(v0,v1) · [0,stride] = exp(v1)/(exp(v0)+exp(v1)) * stride
                float x1, y1, x2, y2;

                auto dfl_dist = [](float v0, float v1, float stride_val) -> float {
                    float max_v = (v0 > v1) ? v0 : v1;
                    float e0 = std::exp(v0 - max_v);
                    float e1 = std::exp(v1 - max_v);
                    return (e1 / (e0 + e1)) * stride_val;
                };

                if (bbox_channels == 8) {
                    float dist_l = dfl_dist(bboxes[i * 8 + 0], bboxes[i * 8 + 1], stride);
                    float dist_t = dfl_dist(bboxes[i * 8 + 2], bboxes[i * 8 + 3], stride);
                    float dist_r = dfl_dist(bboxes[i * 8 + 4], bboxes[i * 8 + 5], stride);
                    float dist_b = dfl_dist(bboxes[i * 8 + 6], bboxes[i * 8 + 7], stride);

                    // Toạ độ trong ảnh letterbox 640×640 → trừ pad → chia scale
                    x1 = (cx - dist_l - pad_x) * inv_scale;
                    y1 = (cy - dist_t - pad_y) * inv_scale;
                    x2 = (cx + dist_r - pad_x) * inv_scale;
                    y2 = (cy + dist_b - pad_y) * inv_scale;
                } else {
                    float dist_l = bboxes[i * 4 + 0] * stride;
                    float dist_t = bboxes[i * 4 + 1] * stride;
                    float dist_r = bboxes[i * 4 + 2] * stride;
                    float dist_b = bboxes[i * 4 + 3] * stride;

                    x1 = (cx - dist_l - pad_x) * inv_scale;
                    y1 = (cy - dist_t - pad_y) * inv_scale;
                    x2 = (cx + dist_r - pad_x) * inv_scale;
                    y2 = (cy + dist_b - pad_y) * inv_scale;
                }
                
                // Clamp
                x1 = std::max(0.0f, std::min(x1, static_cast<float>(frame.cols)));
                y1 = std::max(0.0f, std::min(y1, static_cast<float>(frame.rows)));
                x2 = std::max(0.0f, std::min(x2, static_cast<float>(frame.cols)));
                y2 = std::max(0.0f, std::min(y2, static_cast<float>(frame.rows)));
                
                if (x2 <= x1 || y2 <= y1) continue;
                
                // Size filter — siết chặt để loại false positive trên texture
                float width = x2 - x1;
                float height = y2 - y1;
                float area = width * height;
                float frame_area = static_cast<float>(frame.cols * frame.rows);

                // Min 16×16 = 256px (face dưới mức này không reliable cho ArcFace).
                if (area < 256.0f || area > frame_area * 0.7f) continue;

                // Real face: aspect ~ 0.6-1.4 (taller than wide, gần vuông).
                // Cũ (0.2-5.0) cho phép face dài như cột đèn → false positive.
                float aspect_ratio = width / height;
                if (aspect_ratio < 0.55f || aspect_ratio > 1.6f) continue;
                
                FaceDetectionResult face;
                face.x1 = x1;
                face.y1 = y1;
                face.x2 = x2;
                face.y2 = y2;
                face.confidence = score;
                
                // Parse landmarks (also unscale through letterbox)
                if (landmark_channels == 20) {
                    for (int j = 0; j < 5; ++j) {
                        float lm_x = landmarks[i * 20 + j * 4 + 0];
                        float lm_y = landmarks[i * 20 + j * 4 + 1];

                        lm_x = (cx + lm_x * stride - pad_x) * inv_scale;
                        lm_y = (cy + lm_y * stride - pad_y) * inv_scale;

                        face.landmarks.points[j * 2] = lm_x;
                        face.landmarks.points[j * 2 + 1] = lm_y;
                    }
                } else if (landmark_channels == 10) {
                    for (int j = 0; j < 5; ++j) {
                        float lm_x = landmarks[i * 10 + j * 2 + 0];
                        float lm_y = landmarks[i * 10 + j * 2 + 1];

                        lm_x = (cx + lm_x * stride - pad_x) * inv_scale;
                        lm_y = (cy + lm_y * stride - pad_y) * inv_scale;

                        face.landmarks.points[j * 2] = lm_x;
                        face.landmarks.points[j * 2 + 1] = lm_y;
                    }
                } else {
                    float center_x = (x1 + x2) / 2;
                    float center_y = (y1 + y2) / 2;
                    for (int j = 0; j < 10; j += 2) {
                        face.landmarks.points[j] = center_x;
                        face.landmarks.points[j + 1] = center_y;
                    }
                }
                
                face.person_id = -1;
                face.name = "Unknown";
                std::memset(face.embedding, 0, sizeof(face.embedding));
                
                faces.push_back(face);
            }
        }
        
        // NMS
        if (!faces.empty()) {
            faces = applyFaceNMS(faces, 0.4f);
        }
        
        // Limit
        if (faces.size() > static_cast<size_t>(config_.max_faces_per_frame)) {
            faces.resize(config_.max_faces_per_frame);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[MultiModelInfer] detectFaces error: " << e.what() << std::endl;
    }
    
    return faces;
}

void MultiModelInfer::extractFaceEmbeddings(const cv::Mat& frame, std::vector<FaceDetectionResult>& faces) {
    if (!arcface_engine_ || !arcface_engine_->isLoaded() || frame.empty()) return;
    
    for (auto& face : faces) {
        try {
            cv::Mat aligned = alignFace(frame, face);
            if (aligned.empty() || aligned.rows < 10 || aligned.cols < 10) {
                std::memset(face.embedding, 0, sizeof(face.embedding));
                continue;
            }
            
            cv::Mat resized;
            cv::resize(aligned, resized, cv::Size(config_.arcface_input_size, config_.arcface_input_size));
            
            cv::Mat rgb;
            cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
            rgb.convertTo(rgb, CV_32F);
            rgb = (rgb / 255.0 - 0.5) * 2.0;  // [-1, 1]
            
            std::vector<float> input_tensor(1 * 3 * config_.arcface_input_size * config_.arcface_input_size);
            std::vector<cv::Mat> channels(3);
            cv::split(rgb, channels);
            
            int channel_size = config_.arcface_input_size * config_.arcface_input_size;
            for (int c = 0; c < 3; c++) {
                std::memcpy(input_tensor.data() + c * channel_size, channels[c].data, channel_size * sizeof(float));
            }
            
            std::vector<float> output_tensor;
            if (!arcface_engine_->infer(input_tensor, output_tensor)) {
                std::memset(face.embedding, 0, sizeof(face.embedding));
                continue;
            }

            // BUG-FACE-EMB-01 (audit 2026-05-08): the previous code accepted
            // any output size and clipped/extended via std::min(size, 512)
            // followed by L2-normalising only the populated prefix. With a
            // 256-dim model swap, the first 256 floats normalise but the last
            // 256 stay zero, then matchFaces() still scores the partial
            // vector against every full 512-dim DB embedding — producing
            // plausible-looking but mathematically wrong similarity scores
            // that can spuriously match the wrong person above threshold.
            // Same operational-lie shape as BUG-INFER-01. Fail loud and zero
            // the embedding so matchFaces() returns "Unknown" rather than a
            // garbage identity.
            if (output_tensor.size() != 512) {
                static std::once_flag dim_warned;
                std::call_once(dim_warned, [&output_tensor]() {
                    std::cerr << "[MultiModelInfer] ArcFace produced unexpected embedding "
                              << "dim=" << output_tensor.size() << " (expected 512). "
                              << "Face matching disabled until a 512-dim model is restored."
                              << std::endl;
                });
                std::memset(face.embedding, 0, sizeof(face.embedding));
                continue;
            }

            std::memcpy(face.embedding, output_tensor.data(), 512 * sizeof(float));

            // L2 normalize
            float norm = 0.0f;
            for (size_t i = 0; i < 512; i++) {
                norm += face.embedding[i] * face.embedding[i];
            }
            norm = std::sqrt(norm);

            if (norm > 1e-6f) {
                for (size_t i = 0; i < 512; i++) {
                    face.embedding[i] /= norm;
                }
            } else {
                // Engine returned a degenerate (~zero-norm) vector. Treat as
                // a failed embedding — anything with a sub-microscopic norm
                // would compare cosine ≈ 0 against everything anyway.
                std::memset(face.embedding, 0, sizeof(face.embedding));
            }
        } catch (const std::exception& e) {
            std::memset(face.embedding, 0, sizeof(face.embedding));
        }
    }
}

void MultiModelInfer::matchFaces(std::vector<FaceDetectionResult>& faces) {
    if (face_database_.empty()) {
        for (auto& face : faces) {
            face.person_id = -1;
            face.name = "Unknown";
        }
        return;
    }
    
    for (auto& face : faces) {
        float max_similarity = -1.0f;
        int matched_id = -1;
        std::string matched_name = "Unknown";
        
        for (const auto& person : face_database_) {
            float similarity = cosineSimilarity(face.embedding, person.embedding.data(), 512);
            if (similarity > max_similarity) {
                max_similarity = similarity;
                matched_id = person.id;
                matched_name = person.name;
            }
        }
        
        face.similarity = max_similarity;
        if (max_similarity >= config_.face_match_threshold) {
            face.person_id = matched_id;
            face.name = matched_name;
        } else {
            face.person_id = -1;
            face.name = "Unknown";
        }
    }
}

cv::Mat MultiModelInfer::alignFace(const cv::Mat& frame, const FaceDetectionResult& face) {
    // Check if landmarks are valid: all-zero means detection had no landmark output
    bool landmarks_valid = false;
    for (int lm_i = 0; lm_i < 10; lm_i++) {
        if (face.landmarks.points[lm_i] != 0.0f) { landmarks_valid = true; break; }
    }
    if (!landmarks_valid) {
        // Fallback to simple crop if landmarks aren't valid
        int x1 = static_cast<int>(face.x1);
        int y1 = static_cast<int>(face.y1);
        int x2 = static_cast<int>(face.x2);
        int y2 = static_cast<int>(face.y2);
        
        int width = x2 - x1;
        int height = y2 - y1;
        int padding_x = static_cast<int>(width * 0.1);
        int padding_y = static_cast<int>(height * 0.1);
        
        x1 = std::max(0, x1 - padding_x);
        y1 = std::max(0, y1 - padding_y);
        x2 = std::min(frame.cols, x2 + padding_x);
        y2 = std::min(frame.rows, y2 + padding_y);
        
        if (x2 <= x1 || y2 <= y1 || x1 < 0 || y1 < 0 || x2 > frame.cols || y2 > frame.rows) {
            return cv::Mat();
        }
        return frame(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
    }

    // ArcFace standard 112x112 reference points
    float dst_pts_arr[5][2] = {
        {38.2946f, 51.6963f},
        {73.5318f, 51.5014f},
        {56.0252f, 71.7366f},
        {41.5493f, 92.3655f},
        {70.7299f, 92.2041f}
    };
    
    // Scale reference points to requested input size (if not 112)
    float scale = static_cast<float>(config_.arcface_input_size) / 112.0f;
    std::vector<cv::Point2f> dst_pts;
    for (int i = 0; i < 5; ++i) {
        dst_pts.push_back(cv::Point2f(dst_pts_arr[i][0] * scale, dst_pts_arr[i][1] * scale));
    }
    
    std::vector<cv::Point2f> src_pts;
    for (int i = 0; i < 5; ++i) {
        src_pts.push_back(cv::Point2f(face.landmarks.points[i * 2], face.landmarks.points[i * 2 + 1]));
    }
    
    // Estimate affine transform
    cv::Mat M = cv::estimateAffinePartial2D(src_pts, dst_pts, cv::noArray(), cv::LMEDS);
    if (M.empty()) {
        // Fallback if estimation fails
        return frame(cv::Rect(std::max(0, (int)face.x1), std::max(0, (int)face.y1), 
                              std::min(frame.cols - (int)face.x1, (int)(face.x2 - face.x1)), 
                              std::min(frame.rows - (int)face.y1, (int)(face.y2 - face.y1)))).clone();
    }
    
    cv::Mat aligned;
    cv::warpAffine(frame, aligned, M, cv::Size(config_.arcface_input_size, config_.arcface_input_size), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return aligned;
}

float MultiModelInfer::cosineSimilarity(const float* a, const float* b, int dim) {
    float dot = 0.0f;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
    }
    return dot;
}

std::vector<FaceDetectionResult> MultiModelInfer::applyFaceNMS(const std::vector<FaceDetectionResult>& faces, float nms_threshold) {
    if (faces.empty()) return {};
    
    std::vector<FaceDetectionResult> sorted = faces;
    std::sort(sorted.begin(), sorted.end(), [](const FaceDetectionResult& a, const FaceDetectionResult& b) {
        return a.confidence > b.confidence;
    });
    
    std::vector<FaceDetectionResult> result;
    std::vector<bool> suppressed(sorted.size(), false);
    
    for (size_t i = 0; i < sorted.size(); i++) {
        if (suppressed[i]) continue;
        result.push_back(sorted[i]);
        
        for (size_t j = i + 1; j < sorted.size(); j++) {
            if (suppressed[j]) continue;
            if (calculateFaceIoU(sorted[i], sorted[j]) > nms_threshold) {
                suppressed[j] = true;
            }
        }
    }
    
    return result;
}

float MultiModelInfer::calculateFaceIoU(const FaceDetectionResult& a, const FaceDetectionResult& b) {
    float x1 = std::max(a.x1, b.x1);
    float y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2);
    float y2 = std::min(a.y2, b.y2);
    
    if (x2 <= x1 || y2 <= y1) return 0.0f;
    
    float intersection = (x2 - x1) * (y2 - y1);
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float union_area = area_a + area_b - intersection;
    
    return union_area > 0 ? intersection / union_area : 0.0f;
}

bool MultiModelInfer::addPerson(int id, const std::string& name, const std::vector<float>& embedding) {
    if (embedding.size() != 512 || name.empty()) return false;

    // BUG-AIW-FACEDB-CAP-01 (P0 fix 2026-05-10): pre-fix face_database_ grew
    // unbounded. Each PersonEntry holds a 512-float embedding (2 KB) + name
    // string, so 1M faces ≈ 2.5 GB heap inside every per-camera AI worker.
    // RELOAD_FACE_DB ZMQ command (BUG-AIW-FACEDB-01 wiring) replays the whole
    // SQLite `persons` table on every CUD op, so a misconfigured DB or a
    // future bulk-import endpoint could OOM the worker without a single
    // log line. Cap at 100K by default (~250 MB resident, well under any
    // reasonable face-recognition deployment) with env override for sites
    // that genuinely need more. Hard-fail-loud at the cap so the operator
    // sees the rejection in cpp_backend.log instead of silent partial load.
    static const size_t kMaxFaceDbEntries = []() {
        const char* e = std::getenv("VMS_MAX_FACE_DB_ENTRIES");
        if (e && *e) {
            try {
                long v = std::stol(e);
                if (v > 0 && v < 10'000'000) return static_cast<size_t>(v);
            } catch (...) {}
        }
        return static_cast<size_t>(100'000);
    }();
    if (face_database_.size() >= kMaxFaceDbEntries) {
        static thread_local int warned = 0;
        if ((warned++ % 100) == 0) {
            std::cerr << "[MultiModelInfer] face_database_ cap reached ("
                      << kMaxFaceDbEntries << " entries) — rejecting addPerson id=" << id
                      << " name='" << name << "'. Override via VMS_MAX_FACE_DB_ENTRIES."
                      << std::endl;
        }
        return false;
    }

    PersonEntry person;
    person.id = id;
    person.name = name;
    person.embedding = embedding;
    face_database_.push_back(person);

    return true;
}

bool MultiModelInfer::removePerson(int person_id) {
    auto it = std::find_if(face_database_.begin(), face_database_.end(),
                          [person_id](const PersonEntry& p) { return p.id == person_id; });
    
    if (it == face_database_.end()) return false;
    face_database_.erase(it);
    return true;
}

void MultiModelInfer::clearDatabase() {
    face_database_.clear();
    next_person_id_ = 0;
}

InferenceMetrics MultiModelInfer::getMetrics() const {
    return metrics_;
}

// ============================================================================
// LPR IMPLEMENTATION
// ============================================================================

const char* LPR_CHARS[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-"
};

std::string MultiModelInfer::decodeLPR(const std::vector<float>& output) {
    // Greedy Decoder for CTC
    // Output shape: [1, 88, 18] or [1, 18, 88] depending on LPRNet export.
    // Standard LPRNet (Maxpool) Output: [Batch, Time, Classes] or [Batch, Classes, Time]?
    
    // Hardcoded for typical LPRNet: 88 classes, 18 timesteps.
    int time_steps = 18;
    int num_classes = 88;
    // Verify size
    if (output.size() != time_steps * num_classes) {
        return ""; 
    }

    std::string res = "";
    int last_char_idx = -1;
    
    for (int t = 0; t < time_steps; ++t) {
        int max_idx = -1;
        float max_val = -1e9;
        
        for (int c = 0; c < num_classes; ++c) {
            float val = output[t * num_classes + c]; // [Time, Class] layout assumption
            if (val > max_val) {
                max_val = val;
                max_idx = c;
            }
        }
        
        if (max_idx != last_char_idx && max_idx < 37) { // 37 is likely blank in small models
             if (max_idx < 26) res += (char)('A' + max_idx);
             else if (max_idx < 36) res += (char)('0' + (max_idx - 26));
             else if (max_idx == 36) res += '-';
        }
        last_char_idx = max_idx;
    }
    return res;
}

void MultiModelInfer::recognizePlates(const cv::Mat& frame, std::vector<LicensePlateResult>& plates) {
    // 1. Detect Plates
    auto boxes = plate_detect_infer_->detectObjects(frame);
    
    for (const auto& box : boxes) {
        // 2. Crop
        int x1 = std::max(0, (int)box.x1);
        int y1 = std::max(0, (int)box.y1);
        int x2 = std::min(frame.cols, (int)box.x2);
        int y2 = std::min(frame.rows, (int)box.y2);
        
        if (x2 <= x1 || y2 <= y1) continue;
        
        cv::Mat plate_img = frame(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
        
        // 3. Preprocess for LPRNet (94x24)
        cv::Mat resized;
        cv::resize(plate_img, resized, cv::Size(94, 24));
        
        std::vector<float> input_tensor(3 * 24 * 94);
        
        cv::Mat rgb;
        cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
        rgb.convertTo(rgb, CV_32F, 1.0/128.0, -1.0); // [-1, 1] normalization common for LPRNet
        
        // CHW Conversion
        std::vector<cv::Mat> channels(3);
        cv::split(rgb, channels);
        int channel_size = 94 * 24;
        for (int c = 0; c < 3; c++) {
            std::memcpy(input_tensor.data() + c * channel_size, channels[c].data, channel_size * sizeof(float));
        }

        // 4. Infer
        std::vector<float> output;
        if (lpr_engine_->infer(input_tensor, output)) {
             std::string text = decodeLPR(output);
             if (text.length() > 2) {
                 LicensePlateResult res;
                 res.x1 = box.x1; res.y1 = box.y1; res.x2 = box.x2; res.y2 = box.y2;
                 res.confidence = box.score;
                 res.text = text;
                 plates.push_back(res);
             }
        }
    }
}

std::vector<float> MultiModelInfer::embedImage(const cv::Mat& image) {
    if (image.empty()) return {};
    
    // 1. Detect
    auto faces = detectFaces(image);
    if (faces.empty()) return {};
    
    // 2. Find largest face (Area = width * height)
    size_t largest_idx = 0;
    float max_area = (faces[0].x2 - faces[0].x1) * (faces[0].y2 - faces[0].y1);
    
    for (size_t i = 1; i < faces.size(); ++i) {
        float area = (faces[i].x2 - faces[i].x1) * (faces[i].y2 - faces[i].y1);
        if (area > max_area) {
            max_area = area;
            largest_idx = i;
        }
    }
    
    // 3. Extract embedding for the largest face only
    std::vector<FaceDetectionResult> target_faces;
    target_faces.push_back(faces[largest_idx]);
    extractFaceEmbeddings(image, target_faces);
    
    if (target_faces.empty()) return {};
    
    // Convert array to vector
    std::vector<float> embed(512);
    std::memcpy(embed.data(), target_faces[0].embedding, 512 * sizeof(float));
    return embed;
}

} // namespace inference
