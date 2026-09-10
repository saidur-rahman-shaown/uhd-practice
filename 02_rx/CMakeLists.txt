cmake_minimum_required(VERSION 3.16)

project(uhd_rx_samples
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(UHD REQUIRED)

add_executable(rx_samples
    src/rx_samples.cpp
)

target_link_libraries(rx_samples
    PRIVATE
    UHD::uhd
)
