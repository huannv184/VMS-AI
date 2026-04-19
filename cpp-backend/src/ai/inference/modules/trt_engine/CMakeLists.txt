cmake_minimum_required(VERSION 3.18)

# Find CUDA
find_package(CUDAToolkit REQUIRED)

# Try to find TensorRT
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

if(TensorRT_INCLUDE_DIR AND TensorRT_LIBRARY AND CUDAToolkit_FOUND)
    set(TensorRT_FOUND TRUE)
    message(STATUS "========================================")
    message(STATUS "✓ TensorRT FOUND - Building REAL version")
    message(STATUS "  TensorRT Include: ${TensorRT_INCLUDE_DIR}")
    message(STATUS "  TensorRT Library: ${TensorRT_LIBRARY}")
    message(STATUS "  CUDA Include: ${CUDAToolkit_INCLUDE_DIRS}")
    message(STATUS "  CUDA Version: ${CUDAToolkit_VERSION}")
    message(STATUS "========================================")
    
    # Build with real TensorRT
    add_library(trt_engine STATIC
        trt_engine.cpp
    )
    
    target_include_directories(trt_engine PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${TensorRT_INCLUDE_DIR}
        ${CUDAToolkit_INCLUDE_DIRS}  # ← QUAN TRỌNG: CUDA headers
    )
    
    target_link_libraries(trt_engine PUBLIC
        ${TensorRT_LIBRARY}
        CUDA::cudart
        CUDA::cuda_driver
    )
    
    target_compile_definitions(trt_engine PUBLIC USE_TENSORRT)
    
else()
    message(STATUS "========================================")
    message(STATUS "⚠️  TensorRT or CUDA NOT found - Building STUB version")
    message(STATUS "   TensorRT Include: ${TensorRT_INCLUDE_DIR}")
    message(STATUS "   TensorRT Library: ${TensorRT_LIBRARY}")
    message(STATUS "   CUDA Found: ${CUDAToolkit_FOUND}")
    message(STATUS "   This is OK for development/testing")
    message(STATUS "========================================")
    
    # Build stub version (header-only)
    add_library(trt_engine INTERFACE)
    
    target_include_directories(trt_engine INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    
endif()

set(TensorRT_FOUND ${TensorRT_FOUND} PARENT_SCOPE)