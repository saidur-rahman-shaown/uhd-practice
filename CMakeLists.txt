cmake_minimum_required(VERSION 3.16)

project(uhd_practice
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(UHD REQUIRED)

# Ubuntu's libuhd-dev ships the old-style config that sets only
# UHD_LIBRARIES / UHD_INCLUDE_DIRS and exports no imported target.
if(NOT TARGET UHD::uhd)
    add_library(UHD::uhd INTERFACE IMPORTED)
    target_include_directories(UHD::uhd INTERFACE ${UHD_INCLUDE_DIRS})
    target_link_libraries(UHD::uhd INTERFACE ${UHD_LIBRARIES})
endif()

# Shared setup used by every lesson: Config, create_usrp, the test
# waveform, the async decoders, and the master clock and buffer helpers.
add_library(uhd_common INTERFACE)
target_include_directories(uhd_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(uhd_common INTERFACE UHD::uhd)

# Add one executable, given its lesson directory and source file.
function(uhd_lesson_program name path)
    add_executable(${name} ${path})
    target_link_libraries(${name} PRIVATE uhd_common)
endfunction()

add_subdirectory(01_device)
add_subdirectory(02_rx)
add_subdirectory(03_tx)
add_subdirectory(04_metadata)
add_subdirectory(05_timed_commands)
add_subdirectory(06_tdd)
add_subdirectory(07_sync)
