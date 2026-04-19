#pragma once
#include <cuda_runtime.h>
#include <opencv2/core.hpp>
#include <cstdio>
#include <cstdlib>

namespace vms {

// CUDA error checking macro
#define CUDA_CHECK(call)                                                   \
    do {                                                                   \
        cudaError_t err = call;                                            \
        if (err != cudaSuccess) {                                          \
            fprintf(stderr,                                                \
                    "CUDA error at %s:%d: %s\n",                          \
                    __FILE__, __LINE__, cudaGetErrorString(err));          \
            std::abort();                                                  \
        }                                                                  \
    } while (0)

// GPU memory wrapper
template<typename T>
class GpuBuffer {
public:
    GpuBuffer() : ptr_(nullptr), size_(0) {}
    
    explicit GpuBuffer(size_t count) : ptr_(nullptr), size_(count) {
        if (count > 0) {
            CUDA_CHECK(cudaMalloc(&ptr_, count * sizeof(T)));
        }
    }
    
    ~GpuBuffer() {
        if (ptr_) {
            cudaFree(ptr_);
        }
    }
    
    // Disable copy
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    
    // Enable move
    GpuBuffer(GpuBuffer&& other) noexcept 
        : ptr_(other.ptr_), size_(other.size_) {
        other.ptr_ = nullptr;
        other.size_ = 0;
    }
    
    GpuBuffer& operator=(GpuBuffer&& other) noexcept {
        if (this != &other) {
            if (ptr_) cudaFree(ptr_);
            ptr_ = other.ptr_;
            size_ = other.size_;
            other.ptr_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
    
    T* data() { return ptr_; }
    const T* data() const { return ptr_; }
    size_t size() const { return size_; }
    
    void copyToDevice(const T* host_ptr, size_t count) {
        CUDA_CHECK(cudaMemcpy(ptr_, host_ptr, count * sizeof(T), 
                             cudaMemcpyHostToDevice));
    }
    
    void copyToHost(T* host_ptr, size_t count) const {
        CUDA_CHECK(cudaMemcpy(host_ptr, ptr_, count * sizeof(T), 
                             cudaMemcpyDeviceToHost));
    }
    
private:
    T* ptr_;
    size_t size_;
};

} // namespace vms