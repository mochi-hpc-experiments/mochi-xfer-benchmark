cmake_minimum_required (VERSION 3.15)
project (mochi-xfer-benchmark C CXX)

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
     "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set (CMAKE_CXX_STANDARD 14)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wextra -Wall -Wpedantic")
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wextra -Wall -Wpedantic")

find_package (thallium REQUIRED)
find_package (MPI REQUIRED)
find_package (spdlog REQUIRED)
find_package (PkgConfig REQUIRED)
pkg_check_modules (tclap REQUIRED IMPORTED_TARGET tclap)

add_subdirectory (src)
