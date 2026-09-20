cmake_minimum_required(VERSION 3.16)

# Transmitting samples.
#
# Builds either from the repository root or on its own:
#     cd 03_tx && cmake -B build -G Ninja . && cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(tx LANGUAGES CXX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/uhd_common.cmake)
endif()

uhd_lesson_program(tx_samples src/tx_samples.cpp)
