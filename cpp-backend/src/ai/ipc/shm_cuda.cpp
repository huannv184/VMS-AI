// ipc/shm_cuda.cpp
#include "shm_cuda.h"
#include <cstring>
#include <iostream>

namespace ipc {

bool ShmCuda::create(const char*, size_t bytes) {
    size_ = bytes;
    return true;
}

bool ShmCuda::open(const char*) {
    return true;
}

void ShmCuda::close() {}

bool ShmCuda::copyToDevice(void*, const void*, size_t) { 
    // Stub: CUDA not available or build failed
    return true; 
}
bool ShmCuda::copyToHost(void*, const void*, size_t) { 
    // Stub: CUDA not available or build failed
    return true; 
}
bool ShmCuda::copyDeviceToDevice(void*, const void*, size_t) { 
    // Stub: CUDA not available or build failed
    return true; 
}

} // namespace ipc
