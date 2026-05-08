# inference/CMakeLists.txt

cmake_minimum_required(VERSION 3.18)

# Add subdirectories for modules
add_subdirectory(modules/trt_engine)
add_subdirectory(modules/yolo_infer)
add_subdirectory(modules/reid_infer)
add_subdirectory(modules/custom_model)

# Main inference library
add_library(inference STATIC
    src/advanced_infer.cpp
    src/dummy.cpp
    src/infer_factory.cpp
    src/multi_model_infer.cpp
    src/batch_inference_scheduler.cpp
    src/tracking.cpp
)

target_include_directories(inference PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/include
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/trt_engine
)

target_link_libraries(inference PUBLIC
    trt_engine
    yolo_infer
    reid_infer
    custom_model
    common
    ${OpenCV_LIBS}
)

# Link TensorRT if available
if(TensorRT_FOUND)
    target_link_libraries(inference PUBLIC ${TensorRT_LIBRARIES})
    target_compile_definitions(inference PUBLIC USE_TENSORRT)
endif()

# CUDA support
if(CUDAToolkit_FOUND)
    target_link_libraries(inference PUBLIC CUDA::cudart)
endif()