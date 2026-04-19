// inference/include/inference/batch_inference_scheduler.h
//
// Queue-based batch scheduler for YOLO inference across multiple cameras.
// Camera threads submit frames and receive futures; a dedicated inference
// thread collects up to max_batch frames (or waits max_latency_ms), packs
// them into a single TensorRT batch call, and fulfills per-frame promises.
//
// Thread-safety: submit() is safe to call from any thread.  All GPU work
// happens on the single inference thread — no external synchronization needed.
#pragma once

#include "bounding_box.h"
#include <opencv2/core/mat.hpp>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <functional>

namespace inference {

class TrtEngine;

class BatchInferenceScheduler {
public:
    struct Config {
        std::string engine_path;        // Path to .engine file
        int    max_batch_size   = 4;    // Max frames per GPU dispatch
        int    max_wait_ms      = 5;    // Flush partial batch after this
        int    input_w          = 640;
        int    input_h          = 640;
        float  conf_threshold   = 0.25f;
        float  nms_threshold    = 0.45f;
    };

    explicit BatchInferenceScheduler(const Config& cfg);
    ~BatchInferenceScheduler();

    // Load engine, pre-allocate GPU buffers, start inference thread.
    bool init();

    // Graceful shutdown — finishes pending batch then exits.
    void shutdown();

    // Submit a frame for batched YOLO inference.
    // Returns immediately.  The future yields detections for this frame.
    // Thread-safe.
    std::future<std::vector<BBox>> submit(
        int cam_id, const cv::Mat& frame, uint32_t frame_id);

    // Blocking convenience wrapper.
    std::vector<BBox> submitAndWait(
        int cam_id, const cv::Mat& frame, uint32_t frame_id);

    bool isRunning() const { return running_.load(); }

private:
    // ── Slot representing one submitted frame ──
    struct PendingSlot {
        int          cam_id;
        uint32_t     frame_id;
        cv::Mat      frame;       // BGR, any size — preprocessed on infer thread
        int          orig_w;      // Original width  (for post-process rescaling)
        int          orig_h;      // Original height
        std::promise<std::vector<BBox>> promise;
    };

    // ── Inference thread entry point ──
    void inferenceLoop();

    // ── Process one collected batch ──
    void processBatch(std::vector<PendingSlot>& batch);

    // ── YOLO pre/post-processing (stateless helpers) ──
    void preprocessFrame(const cv::Mat& bgr, float* dst, int target_w, int target_h);

    static std::vector<BBox> parseYoloV8Output(
        const float* output, size_t num_elements,
        float conf_threshold, float nms_threshold,
        int orig_w, int orig_h, int input_w, int input_h);

    static std::vector<BBox> applyNMS(
        const std::vector<BBox>& boxes, float threshold);

    static float computeIOU(const BBox& a, const BBox& b);

    // ── State ──
    Config cfg_;
    std::atomic<bool> running_{false};
    std::thread infer_thread_;

    // ── Thread-safe queue ──
    std::mutex              queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<PendingSlot> pending_;

    // ── TensorRT engine (owned, shared across all cameras) ──
    std::unique_ptr<TrtEngine> engine_;

    // ── Pre-allocated host batch tensor (avoid per-batch allocation) ──
    std::vector<float> batch_input_;   // [max_batch * 3 * H * W]
    std::vector<float> batch_output_;  // [max_batch * per_frame_output_elems]
};

} // namespace inference
