// inference/modules/trt_engine/trt_engine.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <NvInfer.h>
#include <cuda_runtime.h>

namespace inference {

class Logger : public nvinfer1::ILogger {
    void log(Severity severity, const char* msg) noexcept override;
};

class TrtEngine {
public:
    explicit TrtEngine(const std::string& engine_path);
    ~TrtEngine();
    
    // Load engine from file
    bool loadEngine();
    
    // Single input/output inference (return vector)
    std::vector<float> infer(const std::vector<float>& input);
    
    // Single input/output inference (output by reference)
    bool infer(const std::vector<float>& input, std::vector<float>& output);
    
    // Multi-input/output inference
    bool inferMulti(
        const std::vector<std::vector<float>>& inputs,
        std::vector<std::vector<float>>& outputs
    );
    
    // Batch inference — input is [batch_size * C * H * W], output is [batch_size * out_elements].
    // Requires preallocateBatch() to have been called first.
    bool inferBatch(
        const std::vector<float>& input,
        std::vector<float>& output,
        int batch_size
    );

    // Pre-allocate GPU + determine output shape for batch inference.
    // Call once after loadEngine(). Returns false if engine doesn't support dynamic batch.
    bool preallocateBatch(int max_batch_size, int channels, int height, int width);
    void freeBatchBuffers();

    // Number of output elements per single frame (set by preallocateBatch).
    size_t perFrameOutputElements() const { return batch_.per_frame_output_elems; }

    // Getters
    std::vector<int> getInputShape(int index = 0) const;
    std::vector<int> getOutputShape(int index = 0) const;
    std::vector<int> getOutputDims(int index = 0) const;
    int getNumInputs() const { return num_inputs_; }
    int getNumOutputs() const { return num_outputs_; }
    size_t getInputSize(int index = 0) const;
    size_t getOutputSize(int index = 0) const;
    
    bool isLoaded() const { return engine_ != nullptr; }

private:
    std::string engine_path_;
    
    Logger logger_;
    nvinfer1::IRuntime* runtime_ = nullptr;
    nvinfer1::ICudaEngine* engine_ = nullptr;
    nvinfer1::IExecutionContext* context_ = nullptr;
    
    int num_inputs_ = 0;
    int num_outputs_ = 0;
    
    std::vector<void*> buffers_;
    std::vector<size_t> buffer_sizes_;
    std::vector<nvinfer1::Dims> input_dims_;
    std::vector<nvinfer1::Dims> output_dims_;
    std::vector<int> input_indices_;
    std::vector<int> output_indices_;
    std::vector<std::string> tensor_names_;
    std::vector<bool> is_dynamic_;
    
    cudaStream_t stream_;

    // Pre-allocated batch buffers (set by preallocateBatch)
    struct BatchPrealloc {
        void*  d_input  = nullptr;   // GPU input  [max_batch * C * H * W]
        void*  d_output = nullptr;   // GPU output [max_batch * out_elems]
        size_t input_bytes  = 0;
        size_t output_bytes = 0;
        int    max_batch = 0;
        int    C = 0, H = 0, W = 0;
        size_t per_frame_output_elems = 0;
    };
    BatchPrealloc batch_;

    bool allocateBuffers();
    void freeBuffers();
    size_t getDimsSize(const nvinfer1::Dims& dims) const;
    size_t getBindingSize(int index) const;
};

} // namespace inference