# Shared build setup for every lesson.
#
# Included twice over: once by the top-level CMakeLists, which builds all the
# lessons together, and once by a lesson's own CMakeLists when that directory
# is configured on its own. Either of these works:
#
#     cmake -B build -G Ninja .            # from the repository root
#     cd 07_sync && cmake -B build -G Ninja .
#
# so you can rebuild one lesson while working on it without rebuilding the
# rest.

include_guard(GLOBAL)

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

# Config, create_usrp, the test waveform, the async decoders, and the master
# clock and buffer helpers. common/ sits next to this file's directory.
if(NOT TARGET uhd_common)
    add_library(uhd_common INTERFACE)
    target_include_directories(uhd_common
        INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../common)
    target_link_libraries(uhd_common INTERFACE UHD::uhd)
endif()

# Add one executable from a lesson directory.
function(uhd_lesson_program name path)
    add_executable(${name} ${path})
    target_link_libraries(${name} PRIVATE uhd_common)
endfunction()
