cmake_minimum_required(VERSION 3.21)

project(vms_backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Dependencies
find_package(Crow CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(cppzmq CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system filesystem geometry)
find_package(OpenCV REQUIRED)
find_package(GTest REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(pugixml REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Sql Network WebSockets Concurrent)

# Include Directories
include_directories(
    include
    src
    src/ai
    src/ai/video/include
    src/ai/inference/include
    src/ai/inference/third_party/include
    src/ai/common
    src/ai/synopsis
    src/ai/ipc
    src/ai/core_pipeline
    src/ai/inference/modules/custom_model
    src/ai/inference/modules/face_infer
    src/ai/inference/modules/reid_infer
    src/ai/inference/modules/trt_engine
    src/ai/inference/modules/yolo_infer
)

# Source Files
file(GLOB VMS_SOURCES_ROOT "src/*.cpp")
file(GLOB VMS_SOURCES_API "src/api/*.cpp")
file(GLOB VMS_SOURCES_CORE "src/core/*.cpp")
file(GLOB_RECURSE VMS_SOURCES_CORE_NESTED "src/core/**/*.cpp")
file(GLOB VMS_SOURCES_DB "src/database/*.cpp")
file(GLOB VMS_SOURCES_EVENTS "src/events/*.cpp")
file(GLOB_RECURSE VMS_SOURCES_IPC "src/ipc/*.cpp")
file(GLOB VMS_SOURCES_MW "src/middleware/*.cpp")
file(GLOB VMS_SOURCES_REC "src/recording/*.cpp")
file(GLOB VMS_SOURCES_SERVER "src/server/*.cpp")
file(GLOB VMS_SOURCES_STREAM "src/streaming/*.cpp")
file(GLOB VMS_SOURCES_UTILS "src/utils/*.cpp")
file(GLOB_RECURSE VMS_SOURCES_SYNOPSIS "src/ai/synopsis/*.cpp")

set(VMS_SOURCES 
    ${VMS_SOURCES_ROOT}
    ${VMS_SOURCES_API}
    ${VMS_SOURCES_CORE}
    ${VMS_SOURCES_CORE_NESTED}
    ${VMS_SOURCES_DB}
    ${VMS_SOURCES_EVENTS}
    ${VMS_SOURCES_IPC}
    ${VMS_SOURCES_MW}
    ${VMS_SOURCES_REC}
    ${VMS_SOURCES_SERVER}
    ${VMS_SOURCES_STREAM}
    ${VMS_SOURCES_UTILS}
    ${VMS_SOURCES_SYNOPSIS}
    src/core/process_manager.cpp
)

file(GLOB_RECURSE VMS_HEADERS "include/*.h")
list(APPEND VMS_HEADERS
    include/core/process_manager.h
    include/core/runtime_state.h
)

# Main Executable
add_executable(vms_backend ${VMS_SOURCES} ${VMS_HEADERS})
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows")
target_include_directories(vms_backend PRIVATE "${FFMPEG_ROOT}/include")
target_link_directories(vms_backend PRIVATE "${FFMPEG_ROOT}/lib")

target_link_libraries(vms_backend PRIVATE
    Crow::Crow
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    cppzmq
    jwt-cpp::jwt-cpp
    OpenSSL::SSL OpenSSL::Crypto
    Boost::system Boost::filesystem Boost::geometry
    ${OpenCV_LIBS}
    unofficial::sqlite3::sqlite3
    CURL::libcurl
    pugixml
    Qt6::Core
    Qt6::Sql
    Qt6::Network
    Qt6::WebSockets
    Qt6::Concurrent
    avcodec
    avformat
    avutil
    swscale
    swresample
)

# Link LDAP library (Windows only)
if(WIN32)
    target_link_libraries(vms_backend PRIVATE Wldap32)
endif()

# Enable Tests
enable_testing()
# add_subdirectory(tests) # Uncomment if tests folder has CMakeLists.txt

# Definition for Windows lean and mean/nominmax if needed globally
if(MSVC)
    target_compile_definitions(vms_backend PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN BUILD_SHM_READER_C)
endif()

# Force re-configure to pick up moved headers

# AI Worker Executable (separate target, requires TensorRT + CUDA)
add_subdirectory(src/ai_worker)

# Reuse shared AI inference implementation in backend runtime.
target_link_libraries(vms_backend PRIVATE inference)
