cmake_minimum_required(VERSION 3.16)

# What the device reports back about a transfer, and the status characters UHD prints while streaming.
#
# Builds either from the repository root or on its own:
#     cd 04_metadata && cmake -B build -G Ninja . && cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(metadata LANGUAGES CXX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/uhd_common.cmake)
endif()

uhd_lesson_program(async_events src/async_events.cpp)
