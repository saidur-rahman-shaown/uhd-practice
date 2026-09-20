cmake_minimum_required(VERSION 3.16)

# What is attached, and what can it do. Run this first on a new machine.
#
# Builds either from the repository root or on its own:
#     cd 01_device && cmake -B build -G Ninja . && cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(device LANGUAGES CXX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/uhd_common.cmake)
endif()

uhd_lesson_program(device_info src/device_info.cpp)
