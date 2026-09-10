cmake_minimum_required(VERSION 3.16)

project(uhd_device_info
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(UHD REQUIRED)

add_executable(device_info
    src/device_info.cpp
)

target_link_libraries(device_info
    PRIVATE
    UHD::uhd
)
