cmake_minimum_required(VERSION 3.16)

# Zadoff-Chu generation and detection, plus capture and transmit tools for analysing a link offline.
#
# Builds either from the repository root or on its own:
#     cd 07_sync && cmake -B build -G Ninja . && cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(sync LANGUAGES CXX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/uhd_common.cmake)
endif()

uhd_lesson_program(zc_sync src/zc_sync.cpp)
uhd_lesson_program(zc_transmit src/zc_transmit.cpp)
uhd_lesson_program(rx_capture src/rx_capture.cpp)
