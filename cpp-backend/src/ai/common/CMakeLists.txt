# common/CMakeLists.txt

add_library(common STATIC
    common_dummy.cpp  # thêm dummy source
)

target_include_directories(common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)
