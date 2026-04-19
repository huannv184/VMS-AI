add_library(face_infer STATIC face_infer.cpp)

target_include_directories(face_infer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/inference/modules/trt_engine
    ${OpenCV_INCLUDE_DIRS}
    ${TENSORRT_ROOT}/include
)

target_link_libraries(face_infer PUBLIC
    trt_engine ${OpenCV_LIBS}
)