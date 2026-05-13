// inference/modules/trt_engine/trt_engine.cpp
// Thread-safety: Not thread-safe. Each TrtEngine instance must be used
// from a single thread or externally synchronized.
// NOTE: Uses cerr instead of spdlog because this is compiled into ai_worker
// which is a separate executable that doesn't link spdlog.
#include "trt_engine.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// ---- spdlog-compatible shim for ai_worker (no spdlog linkage) ----
//
// BUG-AIW-LOGFMT-01 (P0 fix 2026-05-10): pre-fix the shim accepted a fmt-style
// format string + variadic args but DROPPED every arg, only printing the raw
// format text — so "[TrtEngine] Engine file size: {} bytes" was emitted with
// the literal `{}` placeholder, every TRT diagnostic lost its values. With 4
// AI workers this filled cpp_backend.log with hundreds of useless lines per
// boot and made AV/perf debugging effectively blind. This implements a tiny
// `{}`-substituting formatter — enough to make TrtEngine logs readable,
// without dragging fmt or spdlog into the ai_worker link line.
namespace spdlog {

namespace shim_detail {

template<typename T>
inline std::string to_log_string(T&& v) {
    std::ostringstream os;
    os << std::forward<T>(v);
    return os.str();
}
inline std::string to_log_string(const char* s) {
    return s ? std::string(s) : std::string("(null)");
}
inline std::string to_log_string(std::nullptr_t) { return "(null)"; }

inline void format_into(std::ostringstream& out, const char* fmt) {
    out << fmt; // remaining format text, no more args
}

template<typename T, typename... Rest>
inline void format_into(std::ostringstream& out, const char* fmt, T&& v, Rest&&... rest) {
    if (fmt == nullptr) return;
    const char* p = fmt;
    while (*p) {
        if (p[0] == '{' && p[1] == '}') {
            out.write(fmt, p - fmt);
            out << to_log_string(std::forward<T>(v));
            format_into(out, p + 2, std::forward<Rest>(rest)...);
            return;
        }
        ++p;
    }
    // No `{}` left → flush remaining literal; surplus args are silently
    // discarded to mirror fmt::format's behaviour on argument overflow rather
    // than throw at runtime.
    out << fmt;
}

template<typename... Args>
inline std::string format_message(const char* fmt, Args&&... args) {
    std::ostringstream os;
    format_into(os, fmt ? fmt : "", std::forward<Args>(args)...);
    return os.str();
}

} // namespace shim_detail

template<typename... Args>
inline void info(const char* fmt, Args&&... args) {
    std::cerr << "[INFO] " << shim_detail::format_message(fmt, std::forward<Args>(args)...) << std::endl;
}
template<typename... Args>
inline void error(const char* fmt, Args&&... args) {
    std::cerr << "[ERROR] " << shim_detail::format_message(fmt, std::forward<Args>(args)...) << std::endl;
}
template<typename... Args>
inline void warn(const char* fmt, Args&&... args) {
    std::cerr << "[WARN] " << shim_detail::format_message(fmt, std::forward<Args>(args)...) << std::endl;
}
template<typename... Args>
inline void debug(const char* fmt, Args&&... args) {
    std::cerr << "[DEBUG] " << shim_detail::format_message(fmt, std::forward<Args>(args)...) << std::endl;
}

} // namespace spdlog

namespace inference {

// ================= Logger =================
void Logger::log(Severity severity, const char* msg) noexcept {
    if (severity <= Severity::kWARNING) {
        std::cerr << "[TRT] " << msg << std::endl;
    }
}

// ================= Constructor / Destructor =================
TrtEngine::TrtEngine(const std::string& engine_path)
    : engine_path_(engine_path) {
    cudaStreamCreate(&stream_);
}

TrtEngine::~TrtEngine() {
    freeBatchBuffers();
    freeBuffers();
    if (context_) {
        delete context_;
        context_ = nullptr;
    }
    if (engine_) {
        delete engine_;
        engine_ = nullptr;
    }
    if (runtime_) {
        delete runtime_;
        runtime_ = nullptr;
    }
    cudaStreamDestroy(stream_);
}

// ================= Load Engine =================
bool TrtEngine::loadEngine() {
    spdlog::info("[TrtEngine] Loading engine: {}", engine_path_);

    std::ifstream file(engine_path_, std::ios::binary);
    if (!file.good()) {
        spdlog::error("[TrtEngine] Cannot open engine file: {}", engine_path_);
        return false;
    }

    file.seekg(0, file.end);
    size_t size = file.tellg();
    file.seekg(0, file.beg);

    std::vector<char> engine_data(size);
    file.read(engine_data.data(), size);
    file.close();

    spdlog::info("[TrtEngine] Engine file size: {} bytes", size);

    runtime_ = nvinfer1::createInferRuntime(logger_);
    if (!runtime_) {
        spdlog::error("[TrtEngine] Failed to create runtime");
        return false;
    }

    engine_ = runtime_->deserializeCudaEngine(engine_data.data(), size);
    if (!engine_) {
        spdlog::error("[TrtEngine] Failed to deserialize engine");
        return false;
    }

    context_ = engine_->createExecutionContext();
    if (!context_) {
        spdlog::error("[TrtEngine] Failed to create context");
        return false;
    }

    int32_t num_io_tensors = engine_->getNbIOTensors();
    spdlog::info("[TrtEngine] Total I/O tensors: {}", num_io_tensors);

    num_inputs_ = 0;
    num_outputs_ = 0;
    input_dims_.clear();
    output_dims_.clear();
    input_indices_.clear();
    output_indices_.clear();
    tensor_names_.clear();
    is_dynamic_.clear();

    for (int32_t i = 0; i < num_io_tensors; i++) {
        const char* name_c = engine_->getIOTensorName(i);
        std::string name(name_c);

        nvinfer1::TensorIOMode mode = engine_->getTensorIOMode(name_c);
        nvinfer1::Dims dims = engine_->getTensorShape(name_c);

        tensor_names_.push_back(name);
        
        bool is_dynamic = false;
        for (int j = 0; j < dims.nbDims; j++) {
            if (dims.d[j] == -1) {
                is_dynamic = true;
                break;
            }
        }
        is_dynamic_.push_back(is_dynamic);

        std::string shape_str = "[";
        for (int j = 0; j < dims.nbDims; j++) {
            shape_str += std::to_string(dims.d[j]);
            if (j < dims.nbDims - 1) shape_str += ", ";
        }
        shape_str += "]";
        spdlog::info("[TrtEngine] Tensor[{}]: {} {} {} shape={}",
                     i, name,
                     (mode == nvinfer1::TensorIOMode::kINPUT ? "(INPUT)" : "(OUTPUT)"),
                     (is_dynamic ? "[DYNAMIC]" : "[STATIC]"),
                     shape_str);

        if (mode == nvinfer1::TensorIOMode::kINPUT) {
            input_dims_.push_back(dims);
            input_indices_.push_back(i);
            num_inputs_++;
        } else {
            output_dims_.push_back(dims);
            output_indices_.push_back(i);
            num_outputs_++;
        }
    }

    spdlog::info("[TrtEngine] Inputs: {}, Outputs: {}", num_inputs_, num_outputs_);

    if (!allocateBuffers()) {
        spdlog::error("[TrtEngine] Failed to allocate buffers");
        return false;
    }

    spdlog::info("[TrtEngine] Engine loaded successfully!");
    return true;
}

// ================= Buffer Management =================
bool TrtEngine::allocateBuffers() {
    int32_t num_io_tensors = engine_->getNbIOTensors();
    buffers_.resize(num_io_tensors, nullptr);
    buffer_sizes_.resize(num_io_tensors, 0);

    for (int32_t i = 0; i < num_io_tensors; i++) {
        const char* name_c = tensor_names_[i].c_str();
        
        if (is_dynamic_[i]) {
            spdlog::debug("[TrtEngine] Skipping static allocation for dynamic tensor: {}", name_c);
            buffer_sizes_[i] = 0;
            buffers_[i] = nullptr;
            continue;
        }
        
        nvinfer1::Dims dims = engine_->getTensorShape(name_c);
        size_t size = getDimsSize(dims) * sizeof(float);
        
        if (size == 0 || size > 10ULL * 1024 * 1024 * 1024) {
            spdlog::error("[TrtEngine] Invalid buffer size {} bytes for tensor {}", size, name_c);
            return false;
        }
        
        buffer_sizes_[i] = size;

        if (cudaMalloc(&buffers_[i], size) != cudaSuccess) {
            spdlog::error("[TrtEngine] Failed to allocate buffer {} ({} bytes)", i, size);
            return false;
        }

        spdlog::info("[TrtEngine] Allocated buffer {}: {} bytes ({:.2f} MB)", i, size, static_cast<double>(size) / 1024.0 / 1024.0);
    }

    return true;
}

void TrtEngine::freeBuffers() {
    for (auto buffer : buffers_) {
        if (buffer) cudaFree(buffer);
    }
    buffers_.clear();
    buffer_sizes_.clear();
}

// ================= Utility =================
size_t TrtEngine::getDimsSize(const nvinfer1::Dims& dims) const {
    size_t size = 1;
    for (int i = 0; i < dims.nbDims; i++) {
        if (dims.d[i] <= 0) return 0;
        size *= dims.d[i];
    }
    return size;
}

// ================= Single Inference =================
bool TrtEngine::infer(const std::vector<float>& input, std::vector<float>& output) {
    if (num_inputs_ != 1 || num_outputs_ != 1) {
        spdlog::error("[TrtEngine] Single input/output expected");
        return false;
    }

    int input_idx = input_indices_[0];
    int output_idx = output_indices_[0];

    const char* input_name = tensor_names_[input_idx].c_str();
    const char* output_name = tensor_names_[output_idx].c_str();

    bool input_is_dynamic = is_dynamic_[input_idx];
    bool output_is_dynamic = is_dynamic_[output_idx];

    void* d_input = nullptr;
    void* d_output = nullptr;
    bool allocated_input = false;
    bool allocated_output = false;

    size_t output_size = 0;

    if (input_is_dynamic || output_is_dynamic) {
        size_t total_elements = input.size();
        nvinfer1::Dims new_dims = input_dims_[0];
        
        if (total_elements == 1228800) {
            new_dims.d[0] = 1;
            new_dims.d[1] = 3;
            new_dims.d[2] = 640;
            new_dims.d[3] = 640;
        } else if (total_elements == 37632) {
            new_dims.d[0] = 1;
            new_dims.d[1] = 3;
            new_dims.d[2] = 112;
            new_dims.d[3] = 112;
        } else {
            spdlog::error("[TrtEngine] Unexpected input size: {}", total_elements);
            return false;
        }
        
        if (!context_->setInputShape(input_name, new_dims)) {
            spdlog::error("[TrtEngine] Failed to set input shape");
            return false;
        }
        
        nvinfer1::Dims out_dims = context_->getTensorShape(output_name);
        output_size = 1;
        for (int i = 0; i < out_dims.nbDims; i++) {
            output_size *= out_dims.d[i];
        }
        
        size_t input_bytes = input.size() * sizeof(float);
        if (cudaMalloc(&d_input, input_bytes) != cudaSuccess) {
            spdlog::error("[TrtEngine] Failed to allocate dynamic input buffer");
            return false;
        }
        allocated_input = true;
        
        size_t output_bytes = output_size * sizeof(float);
        if (cudaMalloc(&d_output, output_bytes) != cudaSuccess) {
            spdlog::error("[TrtEngine] Failed to allocate dynamic output buffer");
            cudaFree(d_input);
            return false;
        }
        allocated_output = true;
        
    } else {
        d_input = buffers_[input_idx];
        d_output = buffers_[output_idx];
        output_size = buffer_sizes_[output_idx] / sizeof(float);
        
        if (!d_input || !d_output) {
            spdlog::error("[TrtEngine] Static buffers not allocated");
            return false;
        }
    }

    cudaError_t cuda_status = cudaMemcpyAsync(
        d_input, 
        input.data(),
        input.size() * sizeof(float),
        cudaMemcpyHostToDevice, 
        stream_
    );
    
    if (cuda_status != cudaSuccess) {
        spdlog::error("[TrtEngine] Failed to copy input to GPU: {}", cudaGetErrorString(cuda_status));
        if (allocated_input) cudaFree(d_input);
        if (allocated_output) cudaFree(d_output);
        return false;
    }

    if (!context_->setTensorAddress(input_name, d_input)) {
        spdlog::error("[TrtEngine] Failed to set input tensor address");
        if (allocated_input) cudaFree(d_input);
        if (allocated_output) cudaFree(d_output);
        return false;
    }

    if (!context_->setTensorAddress(output_name, d_output)) {
        spdlog::error("[TrtEngine] Failed to set output tensor address");
        if (allocated_input) cudaFree(d_input);
        if (allocated_output) cudaFree(d_output);
        return false;
    }

    if (!context_->enqueueV3(stream_)) {
        spdlog::error("[TrtEngine] Inference execution failed");
        if (allocated_input) cudaFree(d_input);
        if (allocated_output) cudaFree(d_output);
        return false;
    }

    output.resize(output_size);
    cuda_status = cudaMemcpyAsync(
        output.data(), 
        d_output,
        output_size * sizeof(float),
        cudaMemcpyDeviceToHost, 
        stream_
    );
    
    if (cuda_status != cudaSuccess) {
        spdlog::error("[TrtEngine] Failed to copy output from GPU: {}", cudaGetErrorString(cuda_status));
        if (allocated_input) cudaFree(d_input);
        if (allocated_output) cudaFree(d_output);
        return false;
    }

    cuda_status = cudaStreamSynchronize(stream_);
    if (cuda_status != cudaSuccess) {
        spdlog::error("[TrtEngine] Stream synchronization failed: {}", cudaGetErrorString(cuda_status));
        if (allocated_input) cudaFree(d_input);
        if (allocated_output) cudaFree(d_output);
        return false;
    }

    if (allocated_input) {
        cudaFree(d_input);
    }
    if (allocated_output) {
        cudaFree(d_output);
    }

    return true;
}

// ================= Multi Input/Output Inference - FIXED FOR DYNAMIC TENSORS =================
bool TrtEngine::inferMulti(
    const std::vector<std::vector<float>>& inputs,
    std::vector<std::vector<float>>& outputs) {

    if (inputs.size() != static_cast<size_t>(num_inputs_)) {
        spdlog::error("[TrtEngine] Input count mismatch: expected {}, got {}", num_inputs_, inputs.size());
        return false;
    }

    std::vector<void*> d_input_buffers(num_inputs_, nullptr);
    std::vector<void*> d_output_buffers(num_outputs_, nullptr);
    std::vector<size_t> output_sizes(num_outputs_, 0);
    
    bool allocated_inputs = false;
    bool allocated_outputs = false;

    try {
        // SET INPUT SHAPES FOR DYNAMIC TENSORS
        for (size_t i = 0; i < inputs.size(); i++) {
            int idx = input_indices_[i];
            const char* name = tensor_names_[idx].c_str();
            
            if (is_dynamic_[idx]) {
                size_t total_elements = inputs[i].size();
                nvinfer1::Dims new_dims = input_dims_[i];
                
                if (total_elements == 1228800) {
                    new_dims.d[0] = 1;
                    new_dims.d[1] = 3;
                    new_dims.d[2] = 640;
                    new_dims.d[3] = 640;
                } else if (total_elements == 37632) {
                    new_dims.d[0] = 1;
                    new_dims.d[1] = 3;
                    new_dims.d[2] = 112;
                    new_dims.d[3] = 112;
                } else {
                    spdlog::error("[TrtEngine] Unexpected input size: {}", total_elements);
                    return false;
                }
                
                if (!context_->setInputShape(name, new_dims)) {
                    spdlog::error("[TrtEngine] Failed to set input shape for {}", name);
                    return false;
                }
            }
        }

        // ALLOCATE AND COPY INPUT BUFFERS
        for (size_t i = 0; i < inputs.size(); i++) {
            int idx = input_indices_[i];
            const char* name = tensor_names_[idx].c_str();
            
            size_t input_bytes = inputs[i].size() * sizeof(float);
            
            if (cudaMalloc(&d_input_buffers[i], input_bytes) != cudaSuccess) {
                spdlog::error("[TrtEngine] Failed to allocate input buffer {}", i);
                throw std::runtime_error("CUDA malloc failed");
            }
            
            if (cudaMemcpyAsync(d_input_buffers[i], inputs[i].data(), input_bytes,
                               cudaMemcpyHostToDevice, stream_) != cudaSuccess) {
                spdlog::error("[TrtEngine] Failed to copy input {}", i);
                throw std::runtime_error("CUDA memcpy failed");
            }
            
            if (!context_->setTensorAddress(name, d_input_buffers[i])) {
                spdlog::error("[TrtEngine] Failed to set tensor address for {}", name);
                throw std::runtime_error("Set tensor address failed");
            }
        }
        allocated_inputs = true;

        // GET OUTPUT SHAPES AND ALLOCATE OUTPUT BUFFERS
        for (int i = 0; i < num_outputs_; i++) {
            int idx = output_indices_[i];
            const char* name = tensor_names_[idx].c_str();
            
            nvinfer1::Dims out_dims = context_->getTensorShape(name);
            
            size_t output_size = 1;
            for (int j = 0; j < out_dims.nbDims; j++) {
                if (out_dims.d[j] <= 0) {
                    spdlog::error("[TrtEngine] Invalid output dimension for {}", name);
                    throw std::runtime_error("Invalid output dimension");
                }
                output_size *= out_dims.d[j];
            }
            output_sizes[i] = output_size;
            
            size_t output_bytes = output_size * sizeof(float);
            
            if (cudaMalloc(&d_output_buffers[i], output_bytes) != cudaSuccess) {
                spdlog::error("[TrtEngine] Failed to allocate output buffer {}", i);
                throw std::runtime_error("CUDA malloc failed");
            }
            
            if (!context_->setTensorAddress(name, d_output_buffers[i])) {
                spdlog::error("[TrtEngine] Failed to set output tensor address for {}", name);
                throw std::runtime_error("Set tensor address failed");
            }
        }
        allocated_outputs = true;

        // EXECUTE INFERENCE
        if (!context_->enqueueV3(stream_)) {
            spdlog::error("[TrtEngine] Inference execution failed");
            throw std::runtime_error("Inference failed");
        }

        // COPY OUTPUTS BACK TO HOST
        outputs.resize(num_outputs_);
        for (int i = 0; i < num_outputs_; i++) {
            outputs[i].resize(output_sizes[i]);
            
            size_t output_bytes = output_sizes[i] * sizeof(float);
            if (cudaMemcpyAsync(outputs[i].data(), d_output_buffers[i], output_bytes,
                               cudaMemcpyDeviceToHost, stream_) != cudaSuccess) {
                spdlog::error("[TrtEngine] Failed to copy output {}", i);
                throw std::runtime_error("CUDA memcpy failed");
            }
        }

        // SYNCHRONIZE
        if (cudaStreamSynchronize(stream_) != cudaSuccess) {
            spdlog::error("[TrtEngine] Stream synchronization failed");
            throw std::runtime_error("CUDA sync failed");
        }

    } catch (const std::exception& e) {
        spdlog::error("[TrtEngine] Exception in inferMulti: {}", e.what());
        
        if (allocated_inputs) {
            for (auto ptr : d_input_buffers) {
                if (ptr) cudaFree(ptr);
            }
        }
        if (allocated_outputs) {
            for (auto ptr : d_output_buffers) {
                if (ptr) cudaFree(ptr);
            }
        }
        
        return false;
    }

    // CLEANUP
    for (auto ptr : d_input_buffers) {
        if (ptr) cudaFree(ptr);
    }
    for (auto ptr : d_output_buffers) {
        if (ptr) cudaFree(ptr);
    }

    return true;
}

// ================= Batch Pre-allocation =================

bool TrtEngine::preallocateBatch(int max_batch, int C, int H, int W) {
    if (!context_ || num_inputs_ < 1 || num_outputs_ < 1) {
        spdlog::error("[TrtEngine] Engine not loaded, cannot preallocate batch");
        return false;
    }

    // Verify the input tensor supports dynamic batch (dim 0 == -1)
    int input_idx = input_indices_[0];
    if (!is_dynamic_[input_idx]) {
        spdlog::warn("[TrtEngine] Input tensor is not dynamic — batch limited to static shape");
    }

    const char* input_name  = tensor_names_[input_indices_[0]].c_str();
    const char* output_name = tensor_names_[output_indices_[0]].c_str();

    // Set max batch shape to determine output size
    nvinfer1::Dims in_dims;
    in_dims.nbDims = 4;
    in_dims.d[0] = max_batch;
    in_dims.d[1] = C;
    in_dims.d[2] = H;
    in_dims.d[3] = W;

    if (!context_->setInputShape(input_name, in_dims)) {
        spdlog::error("[TrtEngine] preallocateBatch: setInputShape failed for batch={}", max_batch);
        return false;
    }

    nvinfer1::Dims out_dims = context_->getTensorShape(output_name);
    size_t total_output_elems = 1;
    for (int i = 0; i < out_dims.nbDims; i++) {
        if (out_dims.d[i] <= 0) {
            spdlog::error("[TrtEngine] preallocateBatch: invalid output dim[{}]={}", i, out_dims.d[i]);
            return false;
        }
        total_output_elems *= out_dims.d[i];
    }

    freeBatchBuffers();

    batch_.max_batch = max_batch;
    batch_.C = C;
    batch_.H = H;
    batch_.W = W;
    batch_.input_bytes  = static_cast<size_t>(max_batch) * C * H * W * sizeof(float);
    batch_.output_bytes = total_output_elems * sizeof(float);
    batch_.per_frame_output_elems = total_output_elems / max_batch;

    if (cudaMalloc(&batch_.d_input, batch_.input_bytes) != cudaSuccess) {
        spdlog::error("[TrtEngine] preallocateBatch: cudaMalloc input failed ({} MB)",
                      batch_.input_bytes / (1024.0 * 1024.0));
        return false;
    }
    if (cudaMalloc(&batch_.d_output, batch_.output_bytes) != cudaSuccess) {
        spdlog::error("[TrtEngine] preallocateBatch: cudaMalloc output failed");
        cudaFree(batch_.d_input);
        batch_.d_input = nullptr;
        return false;
    }

    spdlog::info("[TrtEngine] Batch pre-allocated: max_batch={} input={:.1f}MB output={:.1f}MB per_frame_out={}",
                 max_batch,
                 batch_.input_bytes / (1024.0 * 1024.0),
                 batch_.output_bytes / (1024.0 * 1024.0),
                 batch_.per_frame_output_elems);
    return true;
}

void TrtEngine::freeBatchBuffers() {
    if (batch_.d_input)  { cudaFree(batch_.d_input);  batch_.d_input  = nullptr; }
    if (batch_.d_output) { cudaFree(batch_.d_output); batch_.d_output = nullptr; }
    batch_.max_batch = 0;
    batch_.input_bytes = 0;
    batch_.output_bytes = 0;
}

// ================= Batch Inference (pre-allocated) =================

bool TrtEngine::inferBatch(
    const std::vector<float>& input,
    std::vector<float>& output,
    int batch_size)
{
    if (batch_size <= 0) {
        spdlog::error("[TrtEngine] inferBatch: batch_size must be > 0");
        return false;
    }
    if (batch_.max_batch == 0 || !batch_.d_input || !batch_.d_output) {
        spdlog::error("[TrtEngine] inferBatch: call preallocateBatch() first");
        return false;
    }
    if (batch_size > batch_.max_batch) {
        spdlog::error("[TrtEngine] inferBatch: batch_size {} > max {}", batch_size, batch_.max_batch);
        return false;
    }

    const size_t per_frame_in = static_cast<size_t>(batch_.C) * batch_.H * batch_.W;
    const size_t expected_input = static_cast<size_t>(batch_size) * per_frame_in;
    if (input.size() < expected_input) {
        spdlog::error("[TrtEngine] inferBatch: input.size()={} but expected {}", input.size(), expected_input);
        return false;
    }

    const char* input_name  = tensor_names_[input_indices_[0]].c_str();
    const char* output_name = tensor_names_[output_indices_[0]].c_str();

    // 1. Set this batch's input shape [batch_size, C, H, W]
    nvinfer1::Dims in_dims;
    in_dims.nbDims = 4;
    in_dims.d[0] = batch_size;
    in_dims.d[1] = batch_.C;
    in_dims.d[2] = batch_.H;
    in_dims.d[3] = batch_.W;

    if (!context_->setInputShape(input_name, in_dims)) {
        spdlog::error("[TrtEngine] inferBatch: setInputShape failed");
        return false;
    }

    // 2. H2D copy (only the used portion of pre-allocated buffer)
    size_t input_bytes = expected_input * sizeof(float);
    if (cudaMemcpyAsync(batch_.d_input, input.data(), input_bytes,
                        cudaMemcpyHostToDevice, stream_) != cudaSuccess) {
        spdlog::error("[TrtEngine] inferBatch: H2D copy failed");
        return false;
    }

    // 3. Bind addresses
    if (!context_->setTensorAddress(input_name, batch_.d_input) ||
        !context_->setTensorAddress(output_name, batch_.d_output)) {
        spdlog::error("[TrtEngine] inferBatch: setTensorAddress failed");
        return false;
    }

    // 4. Execute
    if (!context_->enqueueV3(stream_)) {
        spdlog::error("[TrtEngine] inferBatch: enqueueV3 failed");
        return false;
    }

    // 5. Determine actual output size and D2H copy
    nvinfer1::Dims out_dims = context_->getTensorShape(output_name);
    size_t total_out = 1;
    for (int i = 0; i < out_dims.nbDims; i++) total_out *= out_dims.d[i];

    output.resize(total_out);
    if (cudaMemcpyAsync(output.data(), batch_.d_output, total_out * sizeof(float),
                        cudaMemcpyDeviceToHost, stream_) != cudaSuccess) {
        spdlog::error("[TrtEngine] inferBatch: D2H copy failed");
        return false;
    }

    // 6. Sync
    if (cudaStreamSynchronize(stream_) != cudaSuccess) {
        spdlog::error("[TrtEngine] inferBatch: sync failed");
        return false;
    }

    return true;
}

// ================= Shape Utilities =================
std::vector<int> TrtEngine::getInputShape(int index) const {
    if (index >= static_cast<int>(input_dims_.size())) return {};
    std::vector<int> shape;
    for (int i = 0; i < input_dims_[index].nbDims; i++) 
        shape.push_back(input_dims_[index].d[i]);
    return shape;
}

std::vector<int> TrtEngine::getOutputShape(int index) const {
    if (index >= static_cast<int>(output_dims_.size())) return {};
    std::vector<int> shape;
    for (int i = 0; i < output_dims_[index].nbDims; i++) 
        shape.push_back(output_dims_[index].d[i]);
    return shape;
}

std::vector<int> TrtEngine::getOutputDims(int index) const {
    return getOutputShape(index);
}

size_t TrtEngine::getInputSize(int index) const {
    if (index >= static_cast<int>(input_dims_.size())) return 0;
    return getDimsSize(input_dims_[index]);
}

size_t TrtEngine::getOutputSize(int index) const {
    if (index >= static_cast<int>(output_dims_.size())) return 0;
    return getDimsSize(output_dims_[index]);
}

} // namespace inference