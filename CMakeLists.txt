cmake_minimum_required(VERSION 3.30 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(enable_standard_modules)
enable_experimental_std()

project(datagram_engine LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "datagram_engine is Linux-only: it is built on recvmmsg, cmsg, GRO/GSO and SO_TXTIME.")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16")
    message(FATAL_ERROR "GCC >= 16 is required. Detected: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

option(DGRAM_BUILD_TESTS    "Build unit tests"             OFF)
option(DGRAM_BUILD_EXAMPLES "Build examples"               OFF)
option(DGRAM_BUILD_FUZZERS  "Build the fuzz harnesses"     OFF)
option(DGRAM_CONTRACTS      "Enable C++26 contract checks" ON)

include(dgram_hardening)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_CXX_MODULE_STD ON)
    set(CMAKE_CXX_SCAN_FOR_MODULES ON)
endif()

if(PROJECT_IS_TOP_LEVEL)
    dgram_apply_hardening()
endif()

# libmem supplies the arena the batches are carved from.
# Offline: -DFETCHCONTENT_SOURCE_DIR_LIBMEM=/path/to/libmem
include(FetchContent)
FetchContent_Declare(libmem
    GIT_REPOSITORY https://github.com/aotodev/libmem.git
    GIT_TAG master
    SYSTEM
)
FetchContent_MakeAvailable(libmem)

add_library(dgram STATIC)

set_target_properties(dgram PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_MODULE_STD ON
    CXX_SCAN_FOR_MODULES ON
)

target_sources(dgram
  PUBLIC
    FILE_SET CXX_MODULES FILES
      src/dgram/error.cppm
      src/dgram/cmsg.cppm
      src/dgram/address.cppm
      src/dgram/socket.cppm
      src/dgram/feature.cppm
      src/dgram/metadata.cppm
      src/dgram/offload.cppm
      src/dgram/batch.cppm
      src/dgram/dgram.cppm
)

target_link_libraries(dgram PUBLIC libmem)

# Contracts live in non-template functions compiled here, so a consumer needs no
# -fcontracts of its own; it does need the violation handler at link time.
if(DGRAM_CONTRACTS)
    target_compile_options(dgram PRIVATE -fcontracts -fcontract-evaluation-semantic=enforce)
    target_link_libraries(dgram PUBLIC stdc++exp)
endif()

add_library(dgram_flags INTERFACE)
target_compile_options(dgram_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast
)
if(PROJECT_IS_TOP_LEVEL)
    target_link_libraries(dgram PRIVATE dgram_flags)
endif()

if(DGRAM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(DGRAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(DGRAM_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
