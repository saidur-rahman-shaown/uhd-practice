cmake_minimum_required(VERSION 3.16)

# Building a frame of alternating transmit and receive slots, and finding what rate the link sustains.
#
# Builds either from the repository root or on its own:
#     cd 06_tdd && cmake -B build -G Ninja . && cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(tdd LANGUAGES CXX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/uhd_common.cmake)
endif()

uhd_lesson_program(tdd_latency_test src/tdd_latency_test.cpp)
