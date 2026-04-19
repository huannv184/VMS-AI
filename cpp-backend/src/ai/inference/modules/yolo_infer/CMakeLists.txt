add_library(yolo_infer STATIC yolo_infer.cpp)

target_include_directories(yolo_infer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/inference/modules/trt_engine
    ${OpenCV_INCLUDE_DIRS}
    ${TENSORRT_ROOT}/include
)

target_link_libraries(yolo_infer PUBLIC
    trt_engine ${OpenCV_LIBS}
)