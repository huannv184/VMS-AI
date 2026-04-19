#include "event_writer_pipeline.h"
#include "detection_management_pipeline.h"
#include "pipeline_manager.h"
#include "tracking_pipeline.h"
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pipeline {

EventWriterPipeline::EventWriterPipeline(const std::string& shm_name)
    : shm_name_(shm_name)
    , shm_handle_(nullptr)
    , shm_ptr_(nullptr)
    , buffer_size_(sizeof(SimpleEvent))
{
    std::cout << "[EventWriterPipeline] Created with buffer size: " 
              << buffer_size_ << " bytes\n";
}

EventWriterPipeline::~EventWriterPipeline() {
    close();
}

bool EventWriterPipeline::initialize() {
#ifdef _WIN32
    DWORD size_high = static_cast<DWORD>((buffer_size_ >> 32) & 0xFFFFFFFF);
    DWORD size_low = static_cast<DWORD>(buffer_size_ & 0xFFFFFFFF);
    
    shm_handle_ = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        size_high,
        size_low,
        shm_name_.c_str()
    );
    
    if (!shm_handle_) {
        std::cerr << "[EventWriterPipeline] CreateFileMapping failed: " 
                  << GetLastError() << "\n";
        return false;
    }
    
    shm_ptr_ = static_cast<uint8_t*>(
        MapViewOfFile(shm_handle_, FILE_MAP_ALL_ACCESS, 0, 0, buffer_size_)
    );
    
    if (!shm_ptr_) {
        std::cerr << "[EventWriterPipeline] MapViewOfFile failed: " 
                  << GetLastError() << "\n";
        CloseHandle(shm_handle_);
        shm_handle_ = nullptr;
        return false;
    }
    
    std::cout << "[EventWriterPipeline] Mapped " << buffer_size_ << " bytes\n";
#else
    int fd = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        std::cerr << "[EventWriterPipeline] shm_open failed\n";
        return false;
    }
    
    if (ftruncate(fd, buffer_size_) == -1) {
        std::cerr << "[EventWriterPipeline] ftruncate failed\n";
        ::close(fd);
        return false;
    }
    
    shm_ptr_ = static_cast<uint8_t*>(
        mmap(nullptr, buffer_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    
    ::close(fd);
    
    if (shm_ptr_ == MAP_FAILED) {
        std::cerr << "[EventWriterPipeline] mmap failed\n";
        shm_ptr_ = nullptr;
        return false;
    }
    
    std::cout << "[EventWriterPipeline] Mapped " << buffer_size_ << " bytes (Linux)\n";
#endif
    
    // Clear memory
    std::memset(shm_ptr_, 0, buffer_size_);
    return true;
}

void EventWriterPipeline::close() {
#ifdef _WIN32
    if (shm_ptr_) {
        UnmapViewOfFile(shm_ptr_);
        shm_ptr_ = nullptr;
    }
    if (shm_handle_) {
        CloseHandle(shm_handle_);
        shm_handle_ = nullptr;
    }
#else
    if (shm_ptr_) {
        munmap(shm_ptr_, buffer_size_);
        shm_ptr_ = nullptr;
    }
#endif
    std::cout << "[EventWriterPipeline] Closed\n";
}

bool EventWriterPipeline::writeEvent(
    uint32_t frame_id,
    uint32_t camera_id,
    const std::vector<inference::BoundingBox>& detections,
    const char* (*get_class_name)(int))
{
    if (!shm_ptr_) {
        return false;
    }
    
    auto* event = reinterpret_cast<SimpleEvent*>(shm_ptr_);
    
    // Fill header
    event->header.magic = 0xDEADBEEF;
    event->header.version = 1;
    event->header.frame_id = frame_id;
    event->header.camera_id = camera_id;
    event->header.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    // Limit to 100 detections
    size_t count = (std::min)(detections.size(), size_t(100));
    event->header.detection_count = static_cast<uint32_t>(count);
    
    // Clear detection array
    std::memset(event->detections, 0, sizeof(event->detections));
    
    // Fill detections
    for (size_t i = 0; i < count; i++) {
        const auto& det = detections[i];
        auto& simple_det = event->detections[i];
        
        simple_det.x1 = det.x1;
        simple_det.y1 = det.y1;
        simple_det.x2 = det.x2;
        simple_det.y2 = det.y2;
        simple_det.confidence = det.score;
        simple_det.class_id = det.class_id;
        
        // Get class name
        const char* class_name = get_class_name ? get_class_name(det.class_id) : "unknown";
        strncpy(simple_det.class_name, class_name, 31);
        simple_det.class_name[31] = '\0';
    }
    
    return true;
}

} // namespace pipeline
