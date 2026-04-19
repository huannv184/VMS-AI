#pragma once
/**
- ppe_detector.hpp
- Module nhận diện áo bảo hộ (PPE) và đồng phục
- VMS-AI Camera System
- 
- Stack: TensorRT 10.x | OpenCV 4.x | ZMQ | nlohmann/json
- Model: YOLOv8 custom-trained (person + ppe classes)
- 
- Classes được detect:
- 0: person
- 1: safety_vest      (áo phản quang / áo bảo hộ)
- 2: hard_hat         (mũ bảo hộ)
- 3: no_safety_vest   (người không có áo bảo hộ – violation)
- 4: no_hard_hat      (người không có mũ – violation)
- 5: uniform_blue     (đồng phục xanh)
- 6: uniform_white    (đồng phục trắng)
- 7: uniform_orange   (đồng phục cam)
- 8: uniform_other    (đồng phục màu khác)
  */

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>

#include <opencv2/opencv.hpp>
#include <NvInfer.h>
#include <nlohmann/json.hpp>

// Forward declarations for opaque types if we don't want cuda headers everywhere
struct CUstream_st;
typedef struct CUstream_st* cudaStream_t;

namespace vms_ai {

// ─────────────────────────────────────────────
// Enums
// ─────────────────────────────────────────────

enum class PPEClass : int {
PERSON           = 0,
SAFETY_VEST      = 1,
HARD_HAT         = 2,
NO_SAFETY_VEST   = 3,   // violation
NO_HARD_HAT      = 4,   // violation
UNIFORM_BLUE     = 5,
UNIFORM_WHITE    = 6,
UNIFORM_ORANGE   = 7,
UNIFORM_OTHER    = 8,
UNKNOWN          = -1
};

enum class ComplianceStatus {
COMPLIANT    = 0,   // Đủ PPE
VIOLATION    = 1,   // Vi phạm (thiếu bảo hộ)
UNCERTAIN    = 2,   // Không xác định được
};

enum class UniformType {
NONE    = 0,
BLUE    = 1,
WHITE   = 2,
ORANGE  = 3,
OTHER   = 4,
};

// ─────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────

struct PPEConfig {
// Model
std::string engine_path      = "models/ppe_yolov8.engine";
int         input_width      = 640;
int         input_height     = 640;
int         num_classes      = 9;
float       conf_threshold   = 0.45f;
float       nms_threshold    = 0.45f;
int         max_detections   = 100;

// Tính năng bật/tắt
bool check_safety_vest = true;   // Kiểm tra áo phản quang
bool check_hard_hat    = true;   // Kiểm tra mũ bảo hộ
bool detect_uniform    = true;   // Nhận diện đồng phục

// ROI (normalized 0..1), empty = toàn frame
std::vector<cv::Rect2f> rois;

// IOU threshold để liên kết PPE với người
float person_ppe_iou_thresh = 0.20f;

// Alert cooldown (ms) – tránh spam cảnh báo
int alert_cooldown_ms = 3000;

// Shared memory key (tích hợp với pipeline VMS-AI)
std::string shm_key = "/vms_ppe";

int         shm_size = 1 << 20;  // 1 MB

// ZMQ endpoint để push alert
std::string zmq_alert_endpoint = "tcp://127.0.0.1:5559";

// Camera ID
std::string camera_id = "cam_01";

static PPEConfig from_json(const nlohmann::json& j);
nlohmann::json   to_json() const;
};

// ─────────────────────────────────────────────
// Detection structs
// ─────────────────────────────────────────────

struct BBox {
float x, y, w, h;   // center_x, center_y, width, height (pixel coords)

cv::Rect to_cv_rect() const {
    return cv::Rect(
        static_cast<int>(x - w / 2),
        static_cast<int>(y - h / 2),
        static_cast<int>(w),
        static_cast<int>(h)
    );
}

float iou(const BBox& other) const;
bool  contains(const BBox& other) const;  // vertical center of other inside this
};

struct RawDetection {
BBox      bbox;
float     confidence;
PPEClass  cls;
};

struct PersonPPEResult {
int             person_idx;     // index trong raw detections
BBox            person_bbox;
float           person_conf;

// PPE được gắn với người này
bool            has_safety_vest = false;
bool            has_hard_hat    = false;
float           vest_conf       = 0.f;
float           hat_conf        = 0.f;

// Đồng phục
UniformType     uniform         = UniformType::NONE;
float           uniform_conf    = 0.f;

ComplianceStatus compliance     = ComplianceStatus::UNCERTAIN;

// Violation detail
std::vector<std::string> violations;  // e.g. {"NO_VEST", "NO_HAT"}
};

// Kết quả trả về cho 1 frame
struct PPEFrameResult {
std::string  camera_id;
int64_t      timestamp_ms;     // epoch ms
int          frame_width;
int          frame_height;

std::vector<RawDetection>   raw_detections;
std::vector<PersonPPEResult> persons;

int total_persons    = 0;
int compliant_count  = 0;
int violation_count  = 0;

float inference_ms   = 0.f;   // thời gian inference
float postproc_ms    = 0.f;   // thời gian postprocess

bool has_violation() const { return violation_count > 0; }
nlohmann::json to_json() const;
};

// Layout shared memory – align với pipeline VMS-AI
#pragma pack(push, 1)
struct PPEShmHeader {
uint32_t magic;              // 0xB0BA1D01
uint32_t version;            // 1
char     camera_id[32];
int64_t  timestamp_ms;
int32_t  total_persons;
int32_t  compliant_count;
int32_t  violation_count;
uint32_t payload_size;       // bytes of JSON payload after header
};
#pragma pack(pop)

// ─────────────────────────────────────────────
// Alert callback
// ─────────────────────────────────────────────

using AlertCallback = std::function<void(const PersonPPEResult&,
const std::string& camera_id,
int64_t timestamp_ms)>;

// ─────────────────────────────────────────────
// TensorRT Logger
// ─────────────────────────────────────────────

class TRTLogger : public nvinfer1::ILogger {
public:
nvinfer1::ILogger::Severity min_severity = nvinfer1::ILogger::Severity::kWARNING;

void log(Severity severity, const char* msg) noexcept override;
};

// ─────────────────────────────────────────────
// PPEDetector – main class
// ─────────────────────────────────────────────

class PPEDetector {
public:
explicit PPEDetector(const PPEConfig& cfg);
~PPEDetector();

// Non-copyable
PPEDetector(const PPEDetector&)            = delete;
PPEDetector& operator=(const PPEDetector&) = delete;

// ── Lifecycle ──────────────────────────────
bool init();     // load engine, alloc CUDA buffers, init shm/zmq
void shutdown();
bool is_ready() const { return ready_.load(); }

// ── Inference ──────────────────────────────
/**
 * Xử lý 1 frame BGR (OpenCV).
 * Thread-safe: có thể gọi từ nhiều thread với mutex.
 */
PPEFrameResult process(const cv::Mat& bgr_frame);

/**
 * Vẽ kết quả lên frame (in-place).
 * Màu: xanh lá = compliant, đỏ = violation, vàng = uncertain.
 */
void draw(cv::Mat& bgr_frame, const PPEFrameResult& result) const;

// ── Config hot-reload ───────────────────────
void update_config(const PPEConfig& new_cfg);
const PPEConfig& config() const { return cfg_; }

// ── Callbacks ──────────────────────────────
void set_alert_callback(AlertCallback cb) { alert_cb_ = std::move(cb); }

// ── Stats ──────────────────────────────────
struct Stats {
    uint64_t total_frames     = 0;
    uint64_t total_violations = 0;
    float    avg_inference_ms = 0.f;
};
Stats get_stats() const;

private:
// ── TensorRT ───────────────────────────────
bool  load_engine();
bool  alloc_cuda_buffers();
bool  run_inference(const float* input_data, int batch = 1);
void  free_cuda_buffers();

// ── Pre/Post processing ────────────────────
cv::Mat     preprocess(const cv::Mat& bgr);
std::vector<RawDetection> postprocess(int img_w, int img_h);

std::vector<RawDetection> apply_nms(std::vector<RawDetection>& dets) const;

// ── ROI filter ─────────────────────────────
bool in_any_roi(const BBox& bbox, int img_w, int img_h) const;

// ── Person-PPE association ──────────────────
std::vector<PersonPPEResult> associate(
    const std::vector<RawDetection>& dets,
    int img_w, int img_h) const;

// ── Compliance logic ───────────────────────
void evaluate_compliance(PersonPPEResult& p) const;

// ── Shared memory ──────────────────────────
bool   init_shm();
void   write_shm(const PPEFrameResult& result);
void   close_shm();

// ── ZMQ alert ──────────────────────────────
bool   init_zmq();
void   send_zmq_alert(const PPEFrameResult& result);
void   close_zmq();

// ── Alert throttle ─────────────────────────
bool   should_alert(const std::string& person_key);

// ── Members ────────────────────────────────
PPEConfig            cfg_;
std::atomic<bool>    ready_{false};
mutable std::mutex   infer_mtx_;

// TensorRT
TRTLogger                                      trt_logger_;
std::unique_ptr<nvinfer1::IRuntime>            runtime_;
std::unique_ptr<nvinfer1::ICudaEngine>         engine_;
std::unique_ptr<nvinfer1::IExecutionContext>   context_;

// CUDA
void*   d_input_  = nullptr;
void*   d_output_ = nullptr;
float*  h_output_ = nullptr;
size_t  input_size_bytes_  = 0;
size_t  output_size_bytes_ = 0;
cudaStream_t stream_ = nullptr;

// Tensor names (YOLOv8 style)
std::string input_name_  = "images";
std::string output_name_ = "output0";
int         output_dims_ = 0;   // 8400 anchors x (4+num_classes)

// Shared memory
int   shm_fd_  = -1;
void* shm_ptr_ = nullptr;

// ZMQ (opaque ptr to avoid zmq.hpp header dependency)
void* zmq_ctx_    = nullptr;
void* zmq_socket_ = nullptr;

// Alert throttle
std::mutex alert_mtx_;
std::unordered_map<std::string,
    std::chrono::steady_clock::time_point> last_alert_time_;

// Callback
AlertCallback alert_cb_;

// Stats
mutable std::mutex stats_mtx_;
Stats stats_;
};

} // namespace vms_ai
