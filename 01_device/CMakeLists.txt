cmake_minimum_required(VERSION 3.16)

project(uhd_device_info
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(UHD REQUIRED)

# Some UHD packages (Ubuntu's) ship the old-style config that only sets
# UHD_LIBRARIES/UHD_INCLUDE_DIRS. Define the imported target when it is missing.
if(NOT TARGET UHD::uhd)
    add_library(UHD::uhd INTERFACE IMPORTED)
    target_include_directories(UHD::uhd INTERFACE ${UHD_INCLUDE_DIRS})
    target_link_libraries(UHD::uhd INTERFACE ${UHD_LIBRARIES})
endif()

add_executable(latency_test
    src/latency_test.cpp
)

target_link_libraries(latency_test
    PRIVATE
    UHD::uhd
)

add_executable(tdd_latency_test
    src/tdd_latency_test.cpp
)

target_link_libraries(tdd_latency_test
    PRIVATE
    UHD::uhd
)

add_executable(async_events
    src/async_events.cpp
)

target_link_libraries(async_events
    PRIVATE
    UHD::uhd
)
