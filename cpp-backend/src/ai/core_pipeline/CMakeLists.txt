project(core_pipeline LANGUAGES CXX)

file(GLOB PIPELINE_SRC *.cpp)
add_library(core_pipeline STATIC ${PIPELINE_SRC})

target_include_directories(core_pipeline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../inference/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../inference/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../video/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../ipc
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(core_pipeline PUBLIC inference video video_publisher ${OpenCV_LIBS})