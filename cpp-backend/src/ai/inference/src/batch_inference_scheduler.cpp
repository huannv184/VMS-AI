// inference/src/batch_inference_scheduler.cpp
#include "inference/batch_inference_scheduler.h"
#include "trt_engine.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>

// COCO class names (same table as advanced_infer.cpp)
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

namespace inference {

// ============================================================
// Construction / Destruction
// ============================================================

BatchInferenceScheduler::BatchInferenceScheduler(const Config& cfg)
    : cfg_(cfg) {}

BatchInferenceScheduler::~BatchInferenceScheduler() {
    shutdown();
}

// ============================================================
// Init — load engine, pre-allocate, start thread
// ============================================================

bool BatchInferenceScheduler::init() {
    if (cfg_.engine_path.empty()) {
        std::cerr << "[BatchScheduler] ERROR: engine_path is empty\n";
        return false;
    }

    // 1. Load TensorRT engine
    engine_ = std::make_unique<TrtEngine>(cfg_.engine_path);
    if (!engine_->loadEngine()) {
        std::cerr << "[BatchScheduler] ERROR: failed to load engine: " << cfg_.engine_path << "\n";
        return false;
    }

    // 2. Pre-allocate GPU buffers for max batch
    if (!engine_->preallocateBatch(cfg_.max_batch_size, 3, cfg_.input_h, cfg_.input_w)) {
        std::cerr << "[BatchScheduler] ERROR: preallocateBatch failed\n";
        return false;
    }

    // 3. Pre-allocate host-side batch tensors
    const size_t per_frame = 3 * cfg_.input_h * cfg_.input_w;
    batch_input_.resize(cfg_.max_batch_size * per_frame);
    batch_output_.resize(cfg_.max_batch_size * engine_->perFrameOutputElements());

    // 4. Start inference thread
    running_ = true;
    infer_thread_ = std::thread(&BatchInferenceScheduler::inferenceLoop, this);

    std::cout << "[BatchScheduler] Ready: max_batch=" << cfg_.max_batch_size
              << " wait=" << cfg_.max_wait_ms << "ms"
              << " input=" << cfg_.input_w << "x" << cfg_.input_h
              << " per_frame_out=" << engine_->perFrameOutputElements() << "\n";
    return true;
}

void BatchInferenceScheduler::shutdown() {
    if (!running_.exchange(false)) return;

    queue_cv_.notify_all();
    if (infer_thread_.joinable()) infer_thread_.join();
    engine_.reset();
}

// ============================================================
// Submit — called from camera threads
// ============================================================

std::future<std::vector<BBox>> BatchInferenceScheduler::submit(
    int cam_id, const cv::Mat& frame, uint32_t frame_id)
{
    PendingSlot slot;
    slot.cam_id   = cam_id;
    slot.frame_id = frame_id;
    slot.frame    = frame.clone();   // Clone — caller's Mat may be reused
    slot.orig_w   = frame.cols;
    slot.orig_h   = frame.rows;

    auto future = slot.promise.get_future();

    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        pending_.push_back(std::move(slot));
    }
    queue_cv_.notify_one();

    return future;
}

std::vector<BBox> BatchInferenceScheduler::submitAndWait(
    int cam_id, const cv::Mat& frame, uint32_t frame_id)
{
    return submit(cam_id, frame, frame_id).get();
}

// ============================================================
// Inference thread — collects batches and dispatches
// ============================================================

void BatchInferenceScheduler::inferenceLoop() {
    const auto max_wait = std::chrono::milliseconds(cfg_.max_wait_ms);

    while (running_) {
        std::vector<PendingSlot> batch;
        batch.reserve(cfg_.max_batch_size);

        // ── Collect up to max_batch_size slots, or flush on timeout ──
        {
            std::unique_lock<std::mutex> lk(queue_mu_);

            // Wait until at least one frame arrives or shutdown
            queue_cv_.wait(lk, [&] { return !pending_.empty() || !running_; });
            if (!running_ && pending_.empty()) break;

            // Drain first frame immediately
            batch.push_back(std::move(pending_.front()));
            pending_.pop_front();

            // Try to fill the rest within max_wait_ms
            if (batch.size() < static_cast<size_t>(cfg_.max_batch_size)) {
                auto deadline = std::chrono::steady_clock::now() + max_wait;

                while (batch.size() < static_cast<size_t>(cfg_.max_batch_size)) {
                    if (queue_cv_.wait_until(lk, deadline, [&] {
                            return !pending_.empty() || !running_;
                        }))
                    {
                        if (!running_ && pending_.empty()) break;
                        while (!pending_.empty() &&
                               batch.size() < static_cast<size_t>(cfg_.max_batch_size))
                        {
                            batch.push_back(std::move(pending_.front()));
                            pending_.pop_front();
                        }
                    } else {
                        break;  // Timeout — flush partial batch
                    }
                }
            }
        }

        if (batch.empty()) continue;

        processBatch(batch);
    }

    // Drain remaining slots on shutdown — set broken promises
    std::lock_guard<std::mutex> lk(queue_mu_);
    for (auto& slot : pending_) {
        slot.promise.set_value({});  // Empty result
    }
    pending_.clear();
}

// ============================================================
// Process one batch — preprocess, infer, postprocess, fulfill
// ============================================================

void BatchInferenceScheduler::processBatch(std::vector<PendingSlot>& batch) {
    const int N = static_cast<int>(batch.size());
    const size_t per_frame_in = 3 * cfg_.input_h * cfg_.input_w;

    // 1. Preprocess each frame → write into batch_input_ (contiguous)
    for (int i = 0; i < N; i++) {
        float* dst = batch_input_.data() + i * per_frame_in;
        preprocessFrame(batch[i].frame, dst, cfg_.input_w, cfg_.input_h);
    }

    // 2. Run TensorRT batch inference
    if (!engine_->inferBatch(batch_input_, batch_output_, N)) {
        std::cerr << "[BatchScheduler] inferBatch failed for batch of " << N << "\n";
        for (auto& slot : batch) slot.promise.set_value({});
        return;
    }

    // 3. Post-process per frame — parse YOLO output + NMS
    const size_t per_frame_out = engine_->perFrameOutputElements();

    for (int i = 0; i < N; i++) {
        const float* frame_output = batch_output_.data() + i * per_frame_out;

        auto boxes = parseYoloV8Output(
            frame_output, per_frame_out,
            cfg_.conf_threshold, cfg_.nms_threshold,
            batch[i].orig_w, batch[i].orig_h,
            cfg_.input_w, cfg_.input_h);

        batch[i].promise.set_value(std::move(boxes));
    }
}

// ============================================================
// Preprocessing — resize + normalize + HWC→CHW into flat buffer
// ============================================================

void BatchInferenceScheduler::preprocessFrame(
    const cv::Mat& bgr, float* dst, int target_w, int target_h)
{
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(target_w, target_h));

    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    cv::Mat float_img;
    rgb.convertTo(float_img, CV_32FC3, 1.0 / 255.0);

    // HWC → CHW directly into dst
    const int hw = target_h * target_w;
    const float* src = reinterpret_cast<const float*>(float_img.data);

    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < hw; i++) {
            dst[c * hw + i] = src[i * 3 + c];
        }
    }
}

// ============================================================
// YOLOv8 output parsing (same logic as AdvancedInfer, standalone)
// ============================================================

std::vector<BBox> BatchInferenceScheduler::parseYoloV8Output(
    const float* output, size_t num_elements,
    float conf_threshold, float nms_threshold,
    int orig_w, int orig_h, int input_w, int input_h)
{
    constexpr int NUM_CLASSES = 80;
    constexpr int NUM_PREDICTIONS = 8400;
    constexpr int OUTPUT_DIMS = 84;  // 4 (bbox) + 80 (classes)

    if (num_elements < NUM_PREDICTIONS * OUTPUT_DIMS) return {};

    const float scale_x = static_cast<float>(orig_w) / input_w;
    const float scale_y = static_cast<float>(orig_h) / input_h;

    std::vector<BBox> boxes;
    boxes.reserve(NUM_PREDICTIONS / 10);

    for (int i = 0; i < NUM_PREDICTIONS; i++) {
        float cx = output[0 * NUM_PREDICTIONS + i];
        float cy = output[1 * NUM_PREDICTIONS + i];
        float w  = output[2 * NUM_PREDICTIONS + i];
        float h  = output[3 * NUM_PREDICTIONS + i];

        float best_conf = 0.0f;
        int best_class = -1;
        for (int c = 0; c < NUM_CLASSES; c++) {
            float conf = output[(4 + c) * NUM_PREDICTIONS + i];
            if (conf > best_conf) {
                best_conf = conf;
                best_class = c;
            }
        }

        if (best_conf < conf_threshold) continue;

        BBox box;
        box.x1 = (cx - w * 0.5f) * scale_x;
        box.y1 = (cy - h * 0.5f) * scale_y;
        box.x2 = (cx + w * 0.5f) * scale_x;
        box.y2 = (cy + h * 0.5f) * scale_y;
        box.score = best_conf;
        box.class_id = best_class;
        box.label = (best_class >= 0 && best_class < 80)
                        ? COCO_CLASSES[best_class] : "unknown";
        boxes.push_back(box);
    }

    return applyNMS(boxes, nms_threshold);
}

std::vector<BBox> BatchInferenceScheduler::applyNMS(
    const std::vector<BBox>& boxes, float threshold)
{
    if (boxes.empty()) return {};

    std::vector<BBox> sorted_boxes = boxes;
    std::sort(sorted_boxes.begin(), sorted_boxes.end(),
              [](const BBox& a, const BBox& b) { return a.score > b.score; });

    std::vector<bool> suppressed(sorted_boxes.size(), false);
    std::vector<BBox> result;

    for (size_t i = 0; i < sorted_boxes.size(); i++) {
        if (suppressed[i]) continue;
        result.push_back(sorted_boxes[i]);

        for (size_t j = i + 1; j < sorted_boxes.size(); j++) {
            if (suppressed[j]) continue;
            if (sorted_boxes[i].class_id == sorted_boxes[j].class_id &&
                computeIOU(sorted_boxes[i], sorted_boxes[j]) > threshold) {
                suppressed[j] = true;
            }
        }
    }
    return result;
}

float BatchInferenceScheduler::computeIOU(const BBox& a, const BBox& b) {
    float x1 = std::max(a.x1, b.x1);
    float y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2);
    float y2 = std::min(a.y2, b.y2);

    float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float union_area = area_a + area_b - intersection;

    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

} // namespace inference
