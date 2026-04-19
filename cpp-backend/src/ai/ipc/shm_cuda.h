// ipc/shm_cuda.h
#pragma once
#include <cstddef>
#include <cstdint>

namespace ipc {

class ShmCuda {
public:
    ShmCuda() = default;
    ~ShmCuda() = default;

    bool create(const char* name, size_t bytes);
    bool open(const char* name);
    void close();

    bool copyToDevice(void* dst, const void* src, size_t bytes);
    bool copyToHost(void* dst, const void* src, size_t bytes);
    bool copyDeviceToDevice(void* dst, const void* src, size_t bytes);

private:
    void* shm_ptr_{nullptr};
    size_t size_{0};
};

} // namespace ipc
