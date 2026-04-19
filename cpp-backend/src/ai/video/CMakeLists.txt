project(video LANGUAGES CXX)

file(GLOB VIDEO_SRC src/*.cpp)
add_library(video STATIC ${VIDEO_SRC})

target_include_directories(video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../inference/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../core_pipeline
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../ipc
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(video PUBLIC inference common ${OpenCV_LIBS})