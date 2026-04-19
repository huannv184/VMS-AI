add_library(custom_model STATIC custom_model.cpp)

target_include_directories(custom_model PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ai/inference/include
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(custom_model PUBLIC ${OpenCV_LIBS})