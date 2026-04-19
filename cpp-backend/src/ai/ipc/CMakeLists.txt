# ipc/CMakeLists.txt

# === IPC core static library ===
add_library(ipc STATIC
    shm_cuda.cpp
    shm_cuda.h
    shm_protocol.h
)

target_include_directories(ipc
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(ipc
    PUBLIC common
)

# Set properties cho static library
set_target_properties(ipc PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    OUTPUT_NAME "ipc"
)

# === SHM Reader C API (DLL for Windows) ===
add_library(shm_reader_c SHARED
    shm_reader_c.cpp
    shm_reader_c.h
)

target_include_directories(shm_reader_c
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# KHÔNG link với ipc library, chỉ cần headers
target_compile_definitions(shm_reader_c
    PRIVATE BUILD_SHM_READER_C
)

# Windows DLL settings
if(WIN32)
    set_target_properties(shm_reader_c PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PREFIX ""
        OUTPUT_NAME "shm_reader_c"
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
    )
endif()

# ============================================================================
# === VIDEO PUBLISHER (ZeroMQ Streaming) - NEW ===
# ============================================================================

# Find ZeroMQ (cppzmq includes libzmq)
find_package(cppzmq CONFIG REQUIRED)

# Video Publisher static library
add_library(video_publisher STATIC
    video_publisher.cpp
    video_publisher.h
)

target_include_directories(video_publisher
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(video_publisher
    PUBLIC
        cppzmq               # ZeroMQ C++ bindings
        opencv_core          # OpenCV core
        opencv_imgcodecs     # For JPEG encoding (imencode)
)

# Set properties
set_target_properties(video_publisher PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    OUTPUT_NAME "video_publisher"
)

# MSVC: /utf-8 required by fmt library
if(MSVC)
    target_compile_options(video_publisher PRIVATE /utf-8)
    target_compile_options(ipc PRIVATE /utf-8)
endif()

# Optional: Create interface library that combines IPC + Video Publisher
# This makes it easier to link both in main application
add_library(ipc_full INTERFACE)
target_link_libraries(ipc_full
    INTERFACE
        ipc
        video_publisher
)