#include "inference/ppe_detector.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>

#include <cuda_runtime_api.h>

namespace vms_ai {

// BBox logic
float BBox::iou(const BBox& other) const {
    float x1 = std::max(x - w / 2, other.x - other.w / 2);
    float y1 = std::max(y - h / 2, other.y - other.h / 2);
    float x2 = std::min(x + w / 2, other.x + other.w / 2);
    float y2 = std::min(y + h / 2, other.y + other.h / 2);

    if (x1 >= x2 || y1 >= y2) return 0.0f;

    float intersection = (x2 - x1) * (y2 - y1);
    float area1 = w * h;
    float area2 = other.w * other.h;

    return intersection / (area1 + area2 - intersection);
}

bool BBox::contains(const BBox& ppe) const {
    float px1 = x - w / 2, px2 = x + w / 2;
    float py1 = y - h / 2, py2 = y + h / 2;

    float pc_x = ppe.x;
    float pc_y = ppe.y;

    return (pc_x > px1) && (pc_x < px2) && (pc_y > py1) && (pc_y < py2);
}

// Config JSON Logic
PPEConfig PPEConfig::from_json(const nlohmann::json& j) {
    PPEConfig c;
    if (j.contains("engine_path")) c.engine_path = j["engine_path"];
    if (j.contains("input_width")) c.input_width = j["input_width"];
    if (j.contains("input_height")) c.input_height = j["input_height"];
    if (j.contains("conf_threshold")) c.conf_threshold = j["conf_threshold"];
    if (j.contains("nms_threshold")) c.nms_threshold = j["nms_threshold"];
    if (j.contains("max_detections")) c.max_detections = j["max_detections"];
    if (j.contains("check_safety_vest")) c.check_safety_vest = j["check_safety_vest"];
    if (j.contains("check_hard_hat")) c.check_hard_hat = j["check_hard_hat"];
    if (j.contains("detect_uniform")) c.detect_uniform = j["detect_uniform"];
    if (j.contains("alert_cooldown_ms")) c.alert_cooldown_ms = j["alert_cooldown_ms"];
    if (j.contains("shm_key")) c.shm_key = j["shm_key"].get<std::string>();
    if (j.contains("zmq_alert_endpoint")) c.zmq_alert_endpoint = j["zmq_alert_endpoint"];
    if (j.contains("camera_id")) c.camera_id = j["camera_id"];
    
    if (j.contains("rois")) {
        for (auto& r : j["rois"]) {
            c.rois.push_back(cv::Rect2f(r[0], r[1], r[2], r[3]));
        }
    }
    return c;
}

nlohmann::json PPEConfig::to_json() const {
    nlohmann::json j;
    j["engine_path"] = engine_path;
    j["input_width"] = input_width;
    j["input_height"] = input_height;
    j["conf_threshold"] = conf_threshold;
    j["nms_threshold"] = nms_threshold;
    j["max_detections"] = max_detections;
    j["check_safety_vest"] = check_safety_vest;
    j["check_hard_hat"] = check_hard_hat;
    j["detect_uniform"] = detect_uniform;
    j["alert_cooldown_ms"] = alert_cooldown_ms;
    j["shm_key"] = shm_key;
    j["zmq_alert_endpoint"] = zmq_alert_endpoint;
    j["camera_id"] = camera_id;
    return j;
}

nlohmann::json PPEFrameResult::to_json() const {
    nlohmann::json j;
    j["camera_id"]       = camera_id;
    j["timestamp_ms"]    = timestamp_ms;
    j["total_persons"]   = total_persons;
    j["compliant_count"] = compliant_count;
    j["violation_count"] = violation_count;
    j["inference_ms"]    = inference_ms;
    j["postproc_ms"]     = postproc_ms;

    nlohmann::json persons_arr = nlohmann::json::array();
    for (auto& p : persons) {
        nlohmann::json pj;
        pj["has_safety_vest"] = p.has_safety_vest;
        pj["has_hard_hat"]    = p.has_hard_hat;
        pj["vest_conf"]       = p.vest_conf;
        pj["hat_conf"]        = p.hat_conf;
        pj["uniform"]         = static_cast<int>(p.uniform);
        pj["uniform_conf"]    = p.uniform_conf;
        pj["compliance"]      = static_cast<int>(p.compliance);
        pj["violations"]      = p.violations;
        pj["bbox"] = {
            {"x", p.person_bbox.x}, {"y", p.person_bbox.y},
            {"w", p.person_bbox.w}, {"h", p.person_bbox.h}
        };
        persons_arr.push_back(std::move(pj));
    }
    j["persons"] = persons_arr;
    return j;
}

void TRTLogger::log(Severity severity, const char* msg) noexcept {
    if (severity > min_severity) return;
    const char* prefix = "[TRT] ";
    switch (severity) {
        case Severity::kERROR:   prefix = "[TRT][ERR] ";   break;
        case Severity::kWARNING: prefix = "[TRT][WARN] ";  break;
        case Severity::kINFO:    prefix = "[TRT][INFO] ";  break;
        default: break;
    }
    fprintf(stderr, "%s%s\n", prefix, msg);
}

PPEDetector::PPEDetector(const PPEConfig& cfg) : cfg_(cfg) {}
PPEDetector::~PPEDetector() { shutdown(); }

bool PPEDetector::init() {
    if (!load_engine())       { fprintf(stderr, "[PPE] load_engine failed\n");  return false; }
    if (!alloc_cuda_buffers()){ fprintf(stderr, "[PPE] alloc_cuda failed\n");   return false; }
    if (!init_shm())          { fprintf(stderr, "[PPE] init_shm failed\n");     return false; }
    if (!init_zmq())          { fprintf(stderr, "[PPE] init_zmq failed\n");     return false; }

    ready_.store(true);
    fprintf(stderr, "[PPE] Detector ready. Camera: %s\n", cfg_.camera_id.c_str());
    return true;
}

void PPEDetector::shutdown() {
    ready_.store(false);
    free_cuda_buffers();
    close_shm();
    close_zmq();

    context_.reset();
    engine_.reset();
    runtime_.reset();
}

bool PPEDetector::load_engine() {
    std::ifstream file(cfg_.engine_path, std::ios::binary | std::ios::ate);
    if (!file.good()) {
        fprintf(stderr, "[PPE] Engine not found: %s\n", cfg_.engine_path.c_str());
        return false;
    }

    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<char> buf(size);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    file.close();

    runtime_.reset(nvinfer1::createInferRuntime(trt_logger_));
    if (!runtime_) return false;

    engine_.reset(runtime_->deserializeCudaEngine(buf.data(), size));
    if (!engine_) return false;

    context_.reset(engine_->createExecutionContext());
    if (!context_) return false;

    auto out_dims = engine_->getTensorShape(output_name_.c_str());
    output_dims_ = out_dims.d[1] * out_dims.d[2];

    return true;
}

bool PPEDetector::alloc_cuda_buffers() {
    int c = 3;
    int h = cfg_.input_height;
    int w = cfg_.input_width;

    input_size_bytes_  = static_cast<size_t>(c * h * w) * sizeof(float);
    output_size_bytes_ = static_cast<size_t>(output_dims_) * sizeof(float);

    if (cudaMalloc(&d_input_,  input_size_bytes_)  != cudaSuccess) return false;
    if (cudaMalloc(&d_output_, output_size_bytes_) != cudaSuccess) return false;

    h_output_ = new float[output_dims_];

    if (cudaStreamCreate(&stream_) != cudaSuccess) return false;

    context_->setTensorAddress(input_name_.c_str(),  d_input_);
    context_->setTensorAddress(output_name_.c_str(), d_output_);

    return true;
}

void PPEDetector::free_cuda_buffers() {
    if (d_input_)  { cudaFree(d_input_);  d_input_  = nullptr; }
    if (d_output_) { cudaFree(d_output_); d_output_ = nullptr; }
    if (h_output_) { delete[] h_output_;  h_output_ = nullptr; }
    if (stream_)   { cudaStreamDestroy(stream_); stream_ = nullptr; }
}

bool PPEDetector::run_inference(const float* input_data, int batch) {
    cudaMemcpyAsync(d_input_, input_data, input_size_bytes_, cudaMemcpyHostToDevice, stream_);
    context_->enqueueV3(stream_);
    cudaMemcpyAsync(h_output_, d_output_, output_size_bytes_, cudaMemcpyDeviceToHost, stream_);
    cudaStreamSynchronize(stream_);
    return true;
}

PPEFrameResult PPEDetector::process(const cv::Mat& bgr_frame) {
    PPEFrameResult result;
    result.camera_id = cfg_.camera_id;
    result.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    result.frame_width = bgr_frame.cols;
    result.frame_height = bgr_frame.rows;

    std::lock_guard<std::mutex> lock(infer_mtx_);

    auto t_start = std::chrono::high_resolution_clock::now();
    cv::Mat input_mat = preprocess(bgr_frame);
    run_inference((float*)input_mat.data);
    auto t_infer = std::chrono::high_resolution_clock::now();

    auto raw_dets = postprocess(bgr_frame.cols, bgr_frame.rows);
    auto final_dets = apply_nms(raw_dets);
    
    result.raw_detections = final_dets;
    result.persons = associate(final_dets, bgr_frame.cols, bgr_frame.rows);

    for (auto& p : result.persons) {
        evaluate_compliance(p);
        if (p.compliance == ComplianceStatus::COMPLIANT) result.compliant_count++;
        if (p.compliance == ComplianceStatus::VIOLATION) result.violation_count++;
    }
    result.total_persons = result.persons.size();

    auto t_post = std::chrono::high_resolution_clock::now();

    result.inference_ms = std::chrono::duration<float, std::milli>(t_infer - t_start).count();
    result.postproc_ms = std::chrono::duration<float, std::milli>(t_post - t_infer).count();

    write_shm(result);
    if (result.has_violation()) {
        send_zmq_alert(result);
    }

    return result;
}

cv::Mat PPEDetector::preprocess(const cv::Mat& bgr) {
    cv::Mat rs, rgb;
    cv::resize(bgr, rs, cv::Size(cfg_.input_width, cfg_.input_height));
    cv::cvtColor(rs, rgb, cv::COLOR_BGR2RGB);
    
    cv::Mat blob;
    rgb.convertTo(blob, CV_32FC3, 1.0f / 255.0f);
    
    // HWC to CHW
    int ch = blob.channels();
    int rows = blob.rows;
    int cols = blob.cols;
    
    cv::Mat flat = blob.reshape(1, rows * cols);
    cv::Mat chw_blob(ch, rows * cols, CV_32FC1);
    
    for (int i = 0; i < ch; ++i) {
        cv::extractChannel(blob, chw_blob.row(i).reshape(1, rows), i);
    }
    
    return chw_blob;
}

std::vector<RawDetection> PPEDetector::postprocess(int img_w, int img_h) {
    std::vector<RawDetection> dets;
    int num_anchors = output_dims_ / (4 + cfg_.num_classes);
    int num_metrics = 4 + cfg_.num_classes;

    float r_w = (float)img_w / cfg_.input_width;
    float r_h = (float)img_h / cfg_.input_height;

    for (int i = 0; i < num_anchors; ++i) {
        float max_conf = 0.0f;
        int max_cls = -1;
        
        for (int c = 0; c < cfg_.num_classes; ++c) {
            float conf = h_output_[c * num_anchors + 4 * num_anchors + i];
            if (conf > max_conf) {
                max_conf = conf;
                max_cls = c;
            }
        }

        if (max_conf >= cfg_.conf_threshold) {
            float cx = h_output_[0 * num_anchors + i] * r_w;
            float cy = h_output_[1 * num_anchors + i] * r_h;
            float w = h_output_[2 * num_anchors + i] * r_w;
            float h = h_output_[3 * num_anchors + i] * r_h;

            dets.push_back({{cx, cy, w, h}, max_conf, static_cast<PPEClass>(max_cls)});
        }
    }
    return dets;
}

std::vector<RawDetection> PPEDetector::apply_nms(std::vector<RawDetection>& dets) const {
    std::vector<RawDetection> result;
    std::sort(dets.begin(), dets.end(), [](const RawDetection& a, const RawDetection& b) {
        return a.confidence > b.confidence;
    });

    std::vector<bool> suppressed(dets.size(), false);
    for (size_t i = 0; i < dets.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(dets[i]);
        if (result.size() >= cfg_.max_detections) break;
        
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (!suppressed[j] && dets[i].cls == dets[j].cls) {
                if (dets[i].bbox.iou(dets[j].bbox) > cfg_.nms_threshold) {
                    suppressed[j] = true;
                }
            }
        }
    }
    return result;
}

std::vector<PersonPPEResult> PPEDetector::associate(const std::vector<RawDetection>& dets, int img_w, int img_h) const {
    std::vector<PersonPPEResult> persons;
    
    // Find all persons
    for (size_t i = 0; i < dets.size(); i++) {
        if (dets[i].cls == PPEClass::PERSON) {
            PersonPPEResult p;
            p.person_idx = i;
            p.person_bbox = dets[i].bbox;
            p.person_conf = dets[i].confidence;
            persons.push_back(p);
        }
    }
    
    // Associate other classes
    for (const auto& d : dets) {
        if (d.cls == PPEClass::PERSON) continue;
        
        // Find best matching person
        float best_iou = 0.0f;
        int best_person_idx = -1;
        
        for (size_t i = 0; i < persons.size(); i++) {
            if (persons[i].person_bbox.contains(d.bbox)) {
                float iou = persons[i].person_bbox.iou(d.bbox);
                if (iou > best_iou) {
                    best_iou = iou;
                    best_person_idx = i;
                }
            }
        }
        
        if (best_person_idx != -1) {
            auto& p = persons[best_person_idx];
            switch (d.cls) {
                case PPEClass::SAFETY_VEST: p.has_safety_vest = true; p.vest_conf = d.confidence; break;
                case PPEClass::HARD_HAT: p.has_hard_hat = true; p.hat_conf = d.confidence; break;
                case PPEClass::UNIFORM_BLUE: p.uniform = UniformType::BLUE; p.uniform_conf = d.confidence; break;
                case PPEClass::UNIFORM_WHITE: p.uniform = UniformType::WHITE; p.uniform_conf = d.confidence; break;
                case PPEClass::UNIFORM_ORANGE: p.uniform = UniformType::ORANGE; p.uniform_conf = d.confidence; break;
                case PPEClass::UNIFORM_OTHER: p.uniform = UniformType::OTHER; p.uniform_conf = d.confidence; break;
                default: break;
            }
        }
    }
    
    return persons;
}

void PPEDetector::evaluate_compliance(PersonPPEResult& p) const {
    p.compliance = ComplianceStatus::COMPLIANT;
    p.violations.clear();
    
    if (cfg_.check_safety_vest && !p.has_safety_vest) {
        p.compliance = ComplianceStatus::VIOLATION;
        p.violations.push_back("NO_SAFETY_VEST");
    }
    
    if (cfg_.check_hard_hat && !p.has_hard_hat) {
        p.compliance = ComplianceStatus::VIOLATION;
        p.violations.push_back("NO_HARD_HAT");
    }
}

void PPEDetector::draw(cv::Mat& bgr_frame, const PPEFrameResult& result) const {
    for (const auto& p : result.persons) {
        cv::Scalar color;
        if (p.compliance == ComplianceStatus::COMPLIANT) color = cv::Scalar(0, 255, 0); // Green
        else if (p.compliance == ComplianceStatus::VIOLATION) color = cv::Scalar(0, 0, 255); // Red
        else color = cv::Scalar(0, 255, 255); // Yellow

        cv::Rect r = p.person_bbox.to_cv_rect();
        cv::rectangle(bgr_frame, r, color, 2);
        
        std::string label = "Person";
        if (p.compliance == ComplianceStatus::VIOLATION) {
            for (const auto& v : p.violations) label += " " + v;
        }
        cv::putText(bgr_frame, label, cv::Point(r.x, r.y - 5), 
                   cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
    }
}

// SHM and ZMQ stubs for now to unblock compilation
bool PPEDetector::init_shm() { return true; }
void PPEDetector::write_shm(const PPEFrameResult& result) {}
void PPEDetector::close_shm() {}

bool PPEDetector::init_zmq() { return true; }
void PPEDetector::send_zmq_alert(const PPEFrameResult& result) {}
void PPEDetector::close_zmq() {}

void PPEDetector::update_config(const PPEConfig& new_cfg) {}
PPEDetector::Stats PPEDetector::get_stats() const { return stats_; }

} // namespace vms_ai
