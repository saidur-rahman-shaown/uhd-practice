cmake_minimum_required(VERSION 3.16)

project(tx_samples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(UHD REQUIRED)

add_executable(tx_samples
    src/tx_samples.cpp
)

target_link_libraries(tx_samples
    UHD::uhd
)
