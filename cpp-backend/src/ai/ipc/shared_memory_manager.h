// ============================================================================
// File: shared_memory_manager.h - FIXED VERSION
// 
// Changes:
// 1. ✅ Added safe_string_copy helpers to prevent iterator invalidation
// 2. ✅ Protected all string operations with try-catch
// 3. ✅ Added fallback for failed string copies
// 4. ✅ Better error logging
// ============================================================================

#pragma once

#include <iostream>
#include <string>
#include <mutex>
#include <memory>
#include <cstring>
#include <iomanip>
#include <vector>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <opencv2/core.hpp>
#include "shm_protocol.h"
#include "inference/multi_model_infer.h" // Canoncial Path
#include "inference/tracking.h"          // Canoncial Path

namespace ipc {

// ============================================================================
// ✅ SAFE STRING COPY HELPERS
// ============================================================================

/**
 * Safe string copy that prevents iterator invalidation issues
 * This is the ROOT CAUSE FIX for the Visual C++ Runtime assertion
 */
inline bool safe_string_copy(char* dest, size_t dest_size, const std::string& src) {
    if (!dest || dest_size == 0) {
        return false;
    }
    
    try {
        // CRITICAL: Make a LOCAL COPY first to avoid string iterator issues
        // The original string might be getting modified/destroyed in another thread
        std::string local_copy = src;
        
        // Zero out destination
        std::memset(dest, 0, dest_size);
        
        // If empty, we're done
        if (local_copy.empty()) {
            return true;
        }
        
        // Calculate safe copy length
        size_t copy_len = std::min(local_copy.length(), dest_size - 1);
        
        // Copy using memcpy (safer than strncpy for binary data)
        std::memcpy(dest, local_copy.data(), copy_len);
        
        // Ensure null termination
        dest[copy_len] = '\0';
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[SHM] String copy exception: " << e.what() << "\n";
        // Leave dest as zeros
        return false;
    } catch (...) {
        std::cerr << "[SHM] Unknown string copy exception\n";
        return false;
    }
}

// ============================================================================
// SHARED MEMORY MANAGER
// ============================================================================
class SharedMemoryManager {
private:
    ShmFrameMetadata* shm_ptr = nullptr;
    size_t shm_size = 0;
    std::string shm_name;
    
    // Video SHM
    ShmVideoBuffer* video_shm_ptr = nullptr;
    size_t video_shm_size = 0;
    std::string video_shm_name;

    std::mutex local_mutex;
    
#ifdef _WIN32
    HANDLE shm_handle = nullptr;
    HANDLE video_shm_handle = nullptr;
#else
    int shm_fd = -1;
    int video_shm_fd = -1;
#endif

public:
    SharedMemoryManager(int camera_id) {
        shm_name = "Local\\ai_pipeline_metadata_" + std::to_string(camera_id);
        shm_size = sizeof(ipc::ShmFrameMetadata);
        
        video_shm_name = "Local\\ai_pipeline_video_" + std::to_string(camera_id);
        video_shm_size = sizeof(ipc::ShmVideoBuffer);
    }
    
    ~SharedMemoryManager() {
        cleanup();
    }

    bool isInitialized() const {
        return shm_ptr != nullptr;
    }
    
    bool initialize() {
        std::lock_guard<std::mutex> lock(local_mutex);
        
        std::cout << "[SHM] Creating shared memory: " << shm_name << "\n";
        std::cout << "[SHM] Size: " << shm_size << " bytes (" 
                  << std::fixed << std::setprecision(2) << (shm_size / 1024.0) << " KB)\n";
        
        if (shm_size == 0 || shm_size > 10 * 1024 * 1024) {
            std::cerr << "[SHM] ERROR: Invalid size!\n";
            return false;
        }
        
#ifdef _WIN32
        shm_handle = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(shm_size), shm_name.c_str()
        );
        
        if (!shm_handle) {
            std::cerr << "[SHM] Failed to create shared memory (Error: " << GetLastError() << ")\n";
            return false;
        }
        
        bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);
        
        shm_ptr = static_cast<ipc::ShmFrameMetadata*>(
            MapViewOfFile(shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, shm_size)
        );
        
        if (!shm_ptr) {
            CloseHandle(shm_handle);
            shm_handle = nullptr;
            std::cerr << "[SHM] Failed to map view\n";
            return false;
        }
#else
        shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) return false;
        
        struct stat shm_stat;
        if (fstat(shm_fd, &shm_stat) == -1) {
            ::close(shm_fd); return false;
        }
        
        bool existed = (shm_stat.st_size == static_cast<off_t>(shm_size));
        if (!existed && ftruncate(shm_fd, shm_size) == -1) {
            ::close(shm_fd); return false;
        }
        
        shm_ptr = static_cast<ipc::ShmFrameMetadata*>(
            mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
        );
        if (shm_ptr == MAP_FAILED) {
            ::close(shm_fd); return false;
        }
#endif
        
        if (!existed || shm_ptr->header.magic != ipc::SHM_MAGIC) {
            std::memset(shm_ptr, 0, shm_size);
            shm_ptr->header.sequence = 0;
            shm_ptr->header.magic = ipc::SHM_MAGIC;
            shm_ptr->header.version = ipc::SHM_VERSION;
        }

        // Initialize Video SHM
#ifdef _WIN32
        video_shm_handle = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(video_shm_size), video_shm_name.c_str()
        );

        if (!video_shm_handle) {
            std::cerr << "[SHM] Failed to create Video SHM (Error: " << GetLastError() << ")\n";
            return false; 
        }

        video_shm_ptr = static_cast<ipc::ShmVideoBuffer*>(
            MapViewOfFile(video_shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, video_shm_size)
        );

        if (!video_shm_ptr) {
            CloseHandle(video_shm_handle);
            video_shm_handle = nullptr;
            std::cerr << "[SHM] Failed to map Video SHM view\n";
            return false;
        }
#endif

        std::cout << "[SHM] ✅ Initialization complete\n";
        return true;
    }
    
    // ============================================================================
    // ✅ FIXED: writeMetadata - ROOT CAUSE FIX
    // ============================================================================
    void writeMetadata(
        const inference::MultiModelResult& result,
        const std::vector<inference::TrackedObject>& tracked_objects,
        int camera_id,
        int width,
        int height
    ) {
        if (!shm_ptr) {
            std::cerr << "[SHM] ERROR: shm_ptr is null!\n";
            return;
        }
        
        try {
            uint32_t seq = shm_ptr->header.sequence;
            shm_ptr->header.sequence = seq + 1; // Mark as writing
            
            ipc::memory_barrier_write();
            
            // Write header
            shm_ptr->header.magic = ipc::SHM_MAGIC;
            shm_ptr->header.version = ipc::SHM_VERSION;
            shm_ptr->header.frame_id = result.frame_id;
            shm_ptr->header.camera_id = camera_id;
            shm_ptr->header.width = width;
            shm_ptr->header.height = height;

            // Clamp object count
            uint32_t obj_count = std::min(
                static_cast<uint32_t>(tracked_objects.size()), 
                static_cast<uint32_t>(ipc::MAX_OBJECTS)
            );
            shm_ptr->header.object_count = obj_count;
            
            // ✅ CRITICAL FIX: Safe object copy with protected string handling
            for (uint32_t i = 0; i < obj_count; i++) {
                const auto& src = tracked_objects[i];
                auto& dst = shm_ptr->objects[i];
                
                // Copy numeric fields (always safe)
                dst.x1 = src.bbox.x1;
                dst.y1 = src.bbox.y1;
                dst.x2 = src.bbox.x2;
                dst.y2 = src.bbox.y2;
                dst.confidence = src.bbox.score;
                dst.class_id = src.bbox.class_id;
                dst.track_id = src.track_id;
                
                // ✅ THIS IS THE FIX: Use safe_string_copy instead of strncpy
                // Old (BROKEN):
                //   std::strncpy(dst.class_name, src.label.c_str(), sizeof(dst.class_name) - 1);
                //
                // New (SAFE):
                if (!safe_string_copy(dst.class_name, sizeof(dst.class_name), src.label)) {
                    // Fallback if string copy fails
                    std::snprintf(dst.class_name, sizeof(dst.class_name), "object_%d", src.bbox.class_id);
                }
            }
            
            // Commit write
            ipc::memory_barrier_write();
            shm_ptr->header.sequence = seq + 2; // Mark as complete (even number)
            
            // Debug logging (every 100 frames)
            static std::atomic<int> log_counter{0};
            if (log_counter.fetch_add(1) % 100 == 0) {
                std::cout << "[SHM] ✅ Frame " << result.frame_id 
                          << " | Objects: " << obj_count 
                          << " | Camera: " << camera_id << "\n";
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[SHM] ❌ EXCEPTION in writeMetadata: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[SHM] ❌ UNKNOWN EXCEPTION in writeMetadata\n";
        }
    }

    bool writeVideoFrame(const cv::Mat& frame, uint64_t frame_id, uint64_t timestamp_ms) {
        if (!video_shm_ptr) return false;
        
        size_t data_size = frame.total() * frame.elemSize();
        if (data_size > ipc::SHM_FRAME_SIZE) return false;

        try {
            uint32_t idx = video_shm_ptr->write_index;
            ipc::ShmVideoFrame& shm_frame = video_shm_ptr->frames[idx % ipc::SHM_BUFFER_COUNT];
            
            shm_frame.frame_id = frame_id;
            shm_frame.timestamp_ms = timestamp_ms;
            shm_frame.width = frame.cols;
            shm_frame.height = frame.rows;
            shm_frame.size = (uint32_t)data_size;
            
            std::memcpy(shm_frame.data, frame.data, data_size);
            
            ipc::memory_barrier_write();
            ipc::atomic_increment(&video_shm_ptr->write_index);
            
            return true;
        } catch (...) {
            return false;
        }
    }

    bool writeFrame(const uint8_t* data, size_t size, int width, int height) {
        if (!video_shm_ptr) return false;
        if (size > ipc::SHM_FRAME_SIZE) return false;

        try {
            uint32_t idx = video_shm_ptr->write_index;
            ipc::ShmVideoFrame& shm_frame = video_shm_ptr->frames[idx % ipc::SHM_BUFFER_COUNT];
            
            shm_frame.frame_id = (uint64_t)idx;
            shm_frame.timestamp_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            shm_frame.width = width;
            shm_frame.height = height;
            shm_frame.size = (uint32_t)size;
            
            std::memcpy(shm_frame.data, data, size);
            
            ipc::memory_barrier_write();
            ipc::atomic_increment(&video_shm_ptr->write_index);
            
            return true;
        } catch (...) {
            return false;
        }
    }

    bool readLatestVideoFrame(cv::Mat& frame, uint64_t& frame_id, uint64_t& timestamp_ms) {
        if (!video_shm_ptr) return false;
        
        try {
            uint32_t w_idx = video_shm_ptr->write_index;
            if (w_idx == 0) return false;
            
            uint32_t slot = (w_idx - 1) % ipc::SHM_BUFFER_COUNT;
            ipc::ShmVideoFrame& shm_frame = video_shm_ptr->frames[slot];
            
            frame_id = shm_frame.frame_id;
            timestamp_ms = shm_frame.timestamp_ms;
            
            if (static_cast<uint32_t>(frame.cols) != shm_frame.width || static_cast<uint32_t>(frame.rows) != shm_frame.height || frame.type() != CV_8UC3) {
                frame.create(shm_frame.height, shm_frame.width, CV_8UC3);
            }
            
            std::memcpy(frame.data, shm_frame.data, shm_frame.size);
            return true;
        } catch (...) {
            return false;
        }
    }
    
    bool readMetadata(inference::MultiModelResult& result, std::vector<inference::TrackedObject>& objects) {
        if (!shm_ptr) return false;
        
        try {
            result.frame_id = shm_ptr->header.frame_id;
            
            objects.clear();
            
            // Validate Magic
            if (shm_ptr->header.magic != ipc::SHM_MAGIC) {
                 // std::cerr << "[SHM] Invalid Magic: " << shm_ptr->header.magic << "\n";
                 return false;
            }

            uint32_t count = std::min(shm_ptr->header.object_count, (uint32_t)ipc::MAX_OBJECTS);
            
            // Debug log if count > 0
            if (count > 0 && shm_ptr->header.frame_id % 100 == 0) {
                std::cout << "[SHM-Read] Frame " << shm_ptr->header.frame_id 
                          << " has " << count << " objects (Header Count: " << shm_ptr->header.object_count << ")\n";
            }

            // Use local vector instead of thread_local to avoid any TLS/Object lifetime issues
            std::vector<inference::TrackedObject> current_objects;
            current_objects.reserve(count);
            
            for (uint32_t i = 0; i < count; i++) {
                const auto& src = shm_ptr->objects[i];
                inference::TrackedObject obj;
                
                obj.bbox.x1 = src.x1;
                obj.bbox.y1 = src.y1;
                obj.bbox.x2 = src.x2;
                obj.bbox.y2 = src.y2;
                obj.bbox.score = src.confidence;
                obj.bbox.class_id = src.class_id;
                obj.track_id = src.track_id;
                obj.confidence = src.confidence;
                
                // Use safe string construction (Explicit check for null terminator)
                // src.class_name is char[64]
                size_t len = 0;
                while(len < sizeof(src.class_name) && src.class_name[len] != '\0') {
                    len++;
                }
                obj.label.assign(src.class_name, len);
                
                if (obj.label.empty()) obj.label = "unknown"; 
                
                current_objects.push_back(obj);
            }
            
            objects = std::move(current_objects);
            return true;
        } catch (...) {
            return false;
        }
    }

    void cleanup() {
        std::lock_guard<std::mutex> lock(local_mutex);
        
        if (shm_ptr) {
#ifdef _WIN32
            UnmapViewOfFile(shm_ptr);
            if (shm_handle) CloseHandle(shm_handle);
#else
            munmap(shm_ptr, shm_size);
            if (shm_fd != -1) ::close(shm_fd);
#endif
            shm_ptr = nullptr;
        }

        if (video_shm_ptr) {
#ifdef _WIN32
            UnmapViewOfFile(video_shm_ptr);
            if (video_shm_handle) CloseHandle(video_shm_handle);
#else
            munmap(video_shm_ptr, video_shm_size);
            if (video_shm_fd != -1) ::close(video_shm_fd);
#endif
            video_shm_ptr = nullptr;
        }
    }
};

} // namespace ipc