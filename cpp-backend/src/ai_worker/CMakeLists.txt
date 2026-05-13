cmake_minimum_required(VERSION 3.18)
project(ai_worker_v2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===========================================
# Dependencies
# ===========================================

# vcpkg packages (from parent project)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(cppzmq CONFIG REQUIRED)
find_package(OpenCV REQUIRED)

# CUDA
find_package(CUDAToolkit REQUIRED)

# TensorRT
find_path(TensorRT_INCLUDE_DIR NvInfer.h
    HINTS
    ${TENSORRT_ROOT}/include
    $ENV{TENSORRT_ROOT}/include
)

find_library(TensorRT_LIBRARY nvinfer
    HINTS
    ${TENSORRT_ROOT}/lib
    $ENV{TENSORRT_ROOT}/lib
)

find_library(TensorRT_PLUGIN_LIBRARY nvinfer_plugin
    HINTS
    ${TENSORRT_ROOT}/lib
    $ENV{TENSORRT_ROOT}/lib
)

if(TensorRT_INCLUDE_DIR AND TensorRT_LIBRARY)
    set(TensorRT_FOUND TRUE)
    message(STATUS "TensorRT FOUND: ${TensorRT_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "TensorRT NOT FOUND - required for AI Worker")
endif()

# ===========================================
# AI Sub-libraries (from src/ai/)
# ===========================================
set(AI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../ai)

add_subdirectory(${AI_ROOT}/common     ${CMAKE_CURRENT_BINARY_DIR}/common)
add_subdirectory(${AI_ROOT}/ipc        ${CMAKE_CURRENT_BINARY_DIR}/ipc)
add_subdirectory(${AI_ROOT}/inference  ${CMAKE_CURRENT_BINARY_DIR}/inference)

# ===========================================
# AI Worker Executable
# ===========================================
add_executable(ai_worker_v2 main.cpp)

target_include_directories(ai_worker_v2 PRIVATE
    ${AI_ROOT}
    ${AI_ROOT}/common
    ${AI_ROOT}/ipc
    ${AI_ROOT}/inference/include
    ${AI_ROOT}/inference/third_party
    ${AI_ROOT}/inference/third_party/include
    ${TensorRT_INCLUDE_DIR}
    ${CUDAToolkit_INCLUDE_DIRS}
)

target_link_libraries(ai_worker_v2 PRIVATE
    inference
    ipc
    common
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    cppzmq
    ${OpenCV_LIBS}
    CUDA::cudart
    # Dbghelp for MiniDumpWriteDump in the SetUnhandledExceptionFilter
    # crash handler (main.cpp install_ai_worker_crash_handler).
    $<$<PLATFORM_ID:Windows>:Dbghelp>
)

if(MSVC)
    target_compile_definitions(ai_worker_v2 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(ai_worker_v2 PRIVATE /utf-8)
    # Emit PDB + cross-reference to it in the EXE so the minidump consumer
    # (WinDbg / VS) can resolve symbols without a separate sympath setup.
    # /MAP also writes a textual symbol→VA map next to the EXE so we can
    # symbolicate AV crash addresses without WinDbg installed (the in-house
    # scripts/symbolize.py uses dbghelp.dll which needs full PDB symbols
    # across all OBJs; /MAP works straight off the linker even when only
    # this target is built with /Zi).
    target_compile_options(ai_worker_v2 PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(ai_worker_v2 PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF /MAP /MAPINFO:EXPORTS>)
endif()

# Output next to vms_backend.exe for both single-config and multi-config generators
set_target_properties(ai_worker_v2 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
)

# Auto copy models next to the executables after build
set(MODEL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../ai/inference/models")
set(MODEL_DST "${CMAKE_BINARY_DIR}/models")
set(MODEL_DST_RELEASE "${CMAKE_BINARY_DIR}/Release/models")

add_custom_command(TARGET ai_worker_v2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODEL_DST}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODEL_DST_RELEASE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MODEL_SRC}/yolo11m.engine"
        "${MODEL_DST}/yolo11m.engine"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MODEL_SRC}/yolo11m.engine"
        "${MODEL_DST_RELEASE}/yolo11m.engine"
    COMMENT "Copying AI model to Release/models/..."
    VERBATIM
)
