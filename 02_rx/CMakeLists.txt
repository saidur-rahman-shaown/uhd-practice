cmake_minimum_required(VERSION 3.16)

# Receiving samples.
#
# Builds either from the repository root or on its own:
#     cd 02_rx && cmake -B build -G Ninja . && cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(rx LANGUAGES CXX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/uhd_common.cmake)
endif()

uhd_lesson_program(rx_samples src/rx_samples.cpp)
