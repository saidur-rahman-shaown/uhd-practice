cmake_minimum_required(VERSION 3.16)

project(uhd_practice
    VERSION 1.0
    LANGUAGES CXX
)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/uhd_common.cmake)

add_subdirectory(01_device)
add_subdirectory(02_rx)
add_subdirectory(03_tx)
add_subdirectory(04_metadata)
add_subdirectory(05_timed_commands)
add_subdirectory(06_tdd)
add_subdirectory(07_sync)
