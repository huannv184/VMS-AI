add_library(reid_infer STATIC reid_infer.cpp)

target_include_directories(reid_infer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/inference/modules/trt_engine
    ${OpenCV_INCLUDE_DIRS}
    ${TENSORRT_ROOT}/include
)

target_link_libraries(reid_infer PUBLIC
    trt_engine ${OpenCV_LIBS}
)