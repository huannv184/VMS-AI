// src/ai/ipc/shm_protocol.h - UNIFIED VERSION (Metadata + Video Buffer)
#pragma once

#ifdef _WIN32
#include <windows.h>   
#include <intrin.h>    
#endif

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ipc {

// Magic number for validation
constexpr uint32_t SHM_MAGIC = 0x46414345;  
constexpr uint32_t SHM_VERSION = 4;          

// Limits
constexpr int MAX_OBJECTS = 50;
constexpr int MAX_FACES = 20;
constexpr int MAX_LANDMARKS = 5;

// ============================================================================
// ENSURE CONSISTENT STRUCT PACKING
// ============================================================================
#pragma pack(push, 1)

// Object Detection
struct ObjectDetection {
    float x1, y1, x2, y2;      
    float confidence;          
    int32_t class_id;          
    int32_t track_id;          
    char class_name[64];       
};

// Facial Landmarks
struct FaceLandmarks {
    float points[10];
};

// Face Detection
struct FaceDetection {
    float x1, y1, x2, y2;
    float confidence;
    FaceLandmarks landmarks;
    float embedding[512];
    int32_t person_id;
    char person_name[64];
};

// Header
struct FrameHeader {
    volatile uint32_t sequence;
    uint32_t magic;
    uint32_t version;
    uint32_t frame_id;
    uint32_t camera_id;
    uint64_t timestamp_ms;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t object_count;
    uint32_t face_count;
    uint32_t yolo_time_us;
    uint32_t face_detect_time_us;
    uint32_t face_recog_time_us;
    uint32_t total_time_us;
    uint8_t reserved[32];
};

// Metadata SHM Structure
struct ShmFrameMetadata {
    FrameHeader header;
    ObjectDetection objects[MAX_OBJECTS];
    FaceDetection faces[MAX_FACES];
};

// ============================================================================
// VIDEO FRAME SHARED MEMORY (RING BUFFER)
// ============================================================================
constexpr int SHM_VIDEO_WIDTH = 640;   
constexpr int SHM_VIDEO_HEIGHT = 360;  
constexpr int SHM_VIDEO_CHANNELS = 3;  
constexpr size_t SHM_FRAME_SIZE = SHM_VIDEO_WIDTH * SHM_VIDEO_HEIGHT * SHM_VIDEO_CHANNELS;
constexpr int SHM_BUFFER_COUNT = 3;    

struct ShmVideoFrame {
    uint64_t frame_id;
    uint64_t timestamp_ms;
    uint32_t width;
    uint32_t height;
    uint32_t size;
    uint8_t data[SHM_FRAME_SIZE]; 
};

struct ShmVideoBuffer {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    ShmVideoFrame frames[SHM_BUFFER_COUNT];
};

#pragma pack(pop)

// Cross-process synchronization helpers
inline void memory_barrier_write() {
#ifdef _WIN32
    _WriteBarrier();
    MemoryBarrier();
#else
    __asm__ __volatile__("" ::: "memory");
    __sync_synchronize();
#endif
}

inline void memory_barrier_read() {
#ifdef _WIN32
    _ReadBarrier();
    MemoryBarrier();
#else
    __asm__ __volatile__("" ::: "memory");
    __sync_synchronize();
#endif
}

inline uint32_t atomic_increment(volatile uint32_t* ptr) {
#ifdef _WIN32
    return (uint32_t)InterlockedIncrement((volatile LONG*)ptr);
#else
    return __sync_add_and_fetch(ptr, 1);
#endif
}

} // namespace ipc