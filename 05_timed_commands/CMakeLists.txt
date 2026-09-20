cmake_minimum_required(VERSION 3.16)

# Host call timing, scheduling lead time, and the metadata that says whether a timed burst actually went out.
#
# Builds either from the repository root or on its own:
#     cd 05_timed_commands && cmake -B build -G Ninja . && cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(timed_commands LANGUAGES CXX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/uhd_common.cmake)
endif()

uhd_lesson_program(latency_test src/latency_test.cpp)
