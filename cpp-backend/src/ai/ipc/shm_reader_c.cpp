#include "shm_reader_c.h"
#include <windows.h>
#include <cstring>
#include <string>
#include <iostream>
#include <map>
#include <mutex>

// ============================================================================
// PRIVATE STATE
// ============================================================================
namespace {
    struct ShmReaderState {
        HANDLE file_mapping;
        ipc::ShmFrameMetadata* mapped_view;
        std::string shm_name;
        int camera_id;
        bool is_open;
        uint32_t last_frame_id;

        ShmReaderState() 
            : file_mapping(nullptr)
            , mapped_view(nullptr)
            , camera_id(-1)
            , is_open(false)
            , last_frame_id(0)
        {}
    };

    // Global state map protected by mutex
    std::map<int, ShmReaderState> g_readers;
    std::mutex g_mutex;
    int g_last_opened_id = -1; // For backward compatibility
}

// ============================================================================
// C API IMPLEMENTATION
// ============================================================================

extern "C" {

SHM_READER_C_API bool open_shm(const char* shm_name, int camera_id) {
    if (!shm_name || strlen(shm_name) == 0) {
        std::cerr << "[SHM Reader] Invalid shm_name\n";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    // Close existing if open for this ID
    if (g_readers.find(camera_id) != g_readers.end()) {
        auto& state = g_readers[camera_id];
        if (state.mapped_view) UnmapViewOfFile(state.mapped_view);
        if (state.file_mapping) CloseHandle(state.file_mapping);
        g_readers.erase(camera_id);
    }

    ShmReaderState state;

    // Open existing shared memory (read-only)
    state.file_mapping = OpenFileMappingA(
        FILE_MAP_READ,          // Read-only access
        FALSE,                  // Don't inherit
        shm_name                // Name
    );

    if (state.file_mapping == nullptr) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            std::cerr << "[SHM Reader] OpenFileMapping failed for " << shm_name << ": " << error << "\n";
        }
        return false;
    }

    // Map view
    state.mapped_view = static_cast<ipc::ShmFrameMetadata*>(
        MapViewOfFile(
            state.file_mapping,
            FILE_MAP_READ,
            0, 0,
            sizeof(ipc::ShmFrameMetadata)
        )
    );

    if (state.mapped_view == nullptr) {
        DWORD error = GetLastError();
        std::cerr << "[SHM Reader] MapViewOfFile failed: " << error << "\n";
        CloseHandle(state.file_mapping);
        return false;
    }

    // Validate magic
    if (state.mapped_view->header.magic != ipc::SHM_MAGIC) {
        std::cerr << "[SHM Reader] Invalid magic number for " << shm_name << "\n";
        UnmapViewOfFile(state.mapped_view);
        CloseHandle(state.file_mapping);
        return false;
    }

    state.shm_name = shm_name;
    state.camera_id = camera_id;
    state.is_open = true;
    state.last_frame_id = 0;

    g_readers[camera_id] = state;
    g_last_opened_id = camera_id;

    std::cout << "[SHM Reader] Connected to: " << shm_name << " (Camera " << camera_id << ")\n";
    return true;
}

SHM_READER_C_API void close_shm() {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Legacy: close all or just last?
    // Let's close all for safety if called without ID (which this API implies)
    // Actually, traditionally close_shm had no args. Ideally we'd have close_shm_id.
    // For now, clear everything to be safe/clean.
    
    for (auto& [id, state] : g_readers) {
        if (state.mapped_view) UnmapViewOfFile(state.mapped_view);
        if (state.file_mapping) CloseHandle(state.file_mapping);
    }
    g_readers.clear();
    g_last_opened_id = -1;
    std::cout << "[SHM Reader] Closed all connections\n";
}

SHM_READER_C_API bool read_metadata_id(int camera_id, ipc::ShmFrameMetadata* out) {
    if (!out) return false;

    // Use a pointer to avoid copying the whole state map or holding lock too long
    // But map pointers are stable.
    ipc::ShmFrameMetadata* view = nullptr;
    uint32_t* last_frame_id_ptr = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_readers.find(camera_id);
        if (it == g_readers.end() || !it->second.is_open || !it->second.mapped_view) {
            return false;
        }
        view = it->second.mapped_view;
        last_frame_id_ptr = &it->second.last_frame_id;
    }

    // No lock needed for reading mapped memory (OS handles coherence mostly, plus we use atomics/volatile logic)
    
    volatile uint32_t magic = view->header.magic;
    if (magic != ipc::SHM_MAGIC) return false;

    // Read frame
    uint32_t frame_id_before = view->header.frame_id;
    std::memcpy(out, view, sizeof(ipc::ShmFrameMetadata));
    uint32_t frame_id_after = view->header.frame_id;

    if (frame_id_before != frame_id_after) {
        std::memcpy(out, view, sizeof(ipc::ShmFrameMetadata));
    }

    // Debug Log
    if (frame_id_after > 0 && frame_id_after % 100 == 0 && frame_id_after != *last_frame_id_ptr) {
        // std::cout << "[SHM Reader] Cam " << camera_id << " | Frame " << frame_id_after << "\n";
        *last_frame_id_ptr = frame_id_after;
    }

    return true;
}

SHM_READER_C_API bool read_metadata(ipc::ShmFrameMetadata* out) {
    int id = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_last_opened_id == -1) return false;
        id = g_last_opened_id;
    }
    
    return read_metadata_id(id, out);
}

SHM_READER_C_API bool is_shm_open() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_readers.empty();
}

SHM_READER_C_API int get_camera_id() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_last_opened_id;
}

} // extern "C"