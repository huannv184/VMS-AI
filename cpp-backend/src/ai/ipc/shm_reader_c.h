#pragma once

#include <cstdint>
#include <stdbool.h>
#include "shm_protocol.h"

// DLL Export/Import macros cho Windows
#ifdef _WIN32
    #ifdef BUILD_SHM_READER_C
        #define SHM_READER_C_API __declspec(dllexport)
    #else
        #define SHM_READER_C_API __declspec(dllimport)
    #endif
#else
    #define SHM_READER_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mở shared memory để đọc metadata
 * 
 * @param shm_name Tên shared memory (VD: "Global\\ai_pipeline_metadata_0")
 * @param camera_id Camera ID (để tracking)
 * @return true nếu thành công, false nếu thất bại
 * 
 * Note: Trên Windows, phải thêm prefix "Global\\" vào tên shared memory
 * VD: "Global\\ai_pipeline_metadata_0"
 */
SHM_READER_C_API bool open_shm(const char* shm_name, int camera_id);

/**
 * Đóng shared memory
 */
SHM_READER_C_API void close_shm();

/**
 * Đọc metadata từ shared memory
 * 
 * @param out Pointer để nhận dữ liệu (phải allocate trước)
 * @return true nếu đọc thành công, false nếu thất bại
 * 
 * Note: Hàm này có race condition protection - nếu data thay đổi 
 * trong quá trình copy, sẽ retry 1 lần
 */
SHM_READER_C_API bool read_metadata(ipc::ShmFrameMetadata* out);

/**
 * Đọc metadata từ shared memory của camera cụ thể
 * 
 * @param camera_id ID của camera cần đọc
 * @param out Pointer để nhận dữ liệu
 * @return true nếu thành công
 */
SHM_READER_C_API bool read_metadata_id(int camera_id, ipc::ShmFrameMetadata* out);

/**
 * Kiểm tra xem shared memory có đang mở không
 * 
 * @return true nếu đang mở, false nếu không
 */
SHM_READER_C_API bool is_shm_open();

/**
 * Lấy camera ID hiện tại
 * 
 * @return camera ID, hoặc -1 nếu chưa mở
 */
SHM_READER_C_API int get_camera_id();

#ifdef __cplusplus
}
#endif