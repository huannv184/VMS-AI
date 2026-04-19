#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <cstring>
#include <chrono>
#include "inference/bounding_box.h"

namespace pipeline {

// ============================================================================
// Simple Event Structures
// ============================================================================

#pragma pack(push, 1)
struct SimpleDetection {
    float x1, y1, x2, y2;
    float confidence;
    int32_t class_id;
    char class_name[32];
};

struct SimpleEventHeader {
    uint32_t magic;           // 0xDEADBEEF
    uint32_t version;         // 1
    uint32_t frame_id;
    uint32_t camera_id;
    uint64_t timestamp_ms;
    uint32_t detection_count;
    uint32_t reserved[10];
};

struct SimpleEvent {
    SimpleEventHeader header;
    SimpleDetection detections[100];  // Max 100 detections
};
#pragma pack(pop)

// ============================================================================
// Event Writer Pipeline
// ============================================================================

class EventWriterPipeline {
public:
    explicit EventWriterPipeline(const std::string& shm_name = "Global\\events_shm");
    ~EventWriterPipeline();

    // Initialize shared memory
    bool initialize();
    
    // Close shared memory
    void close();
    
    // Write detection event
    bool writeEvent(
        uint32_t frame_id,
        uint32_t camera_id,
        const std::vector<inference::BoundingBox>& detections,
        const char* (*get_class_name)(int)
    );
    
    // Check if initialized
    bool isInitialized() const { return shm_ptr_ != nullptr; }
    
    // Get buffer size
    size_t getBufferSize() const { return buffer_size_; }

private:
    std::string shm_name_;
    void* shm_handle_;        // Windows: HANDLE, Linux: unused
    uint8_t* shm_ptr_;
    size_t buffer_size_;
};

} // namespace pipeline
