cmake_minimum_required(VERSION 3.30 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(enable_standard_modules)
enable_experimental_std()

project(datagram_engine VERSION 0.10.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "datagram-engine is Linux-only: it is built on recvmmsg, cmsg, GRO/GSO and SO_TXTIME.")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16")
    message(FATAL_ERROR "GCC >= 16 is required. Detected: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

option(DGRAM_BUILD_TESTS    "Build unit tests"                                OFF)
option(DGRAM_BUILD_EXAMPLES "Build examples"                                  OFF)
option(DGRAM_BUILD_FUZZERS  "Build the fuzz harnesses"                        OFF)
option(DGRAM_INSTALL        "Generate install rules and a find_package config" ${PROJECT_IS_TOP_LEVEL})

include(dgram_hardening)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_MODULE_STD ON)
    set(CMAKE_CXX_SCAN_FOR_MODULES ON)

    # Not forced: the gcc-gnu.yml leg configures with -DCMAKE_CXX_EXTENSIONS=ON.
    if(NOT DEFINED CMAKE_CXX_EXTENSIONS)
        set(CMAKE_CXX_EXTENSIONS OFF)
    endif()
endif()

if(PROJECT_IS_TOP_LEVEL)
    dgram_apply_hardening()
endif()

# libmem supplies the arena the batches are carved from.
# Offline: -DFETCHCONTENT_SOURCE_DIR_LIBMEM=/path/to/libmem
include(FetchContent)
FetchContent_Declare(libmem
    GIT_REPOSITORY https://github.com/aotodev/libmem.git
    GIT_TAG v0.10.0
    SYSTEM
    FIND_PACKAGE_ARGS 0.10.0
)

# A fetched libmem has no export set of its own, and install(EXPORT) refuses a
# target whose dependency is in none. Its module sources have to ship anyway:
# dgram's interfaces `import libmem`, so a consumer builds libmem's BMIs too.
if(DGRAM_INSTALL)
    set(LIBMEM_INSTALL ON CACHE BOOL "" FORCE)
endif()

# A static libmem linked into libdgram.so has to be position independent.
if(BUILD_SHARED_LIBS)
    set(LIBMEM_PIC ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(libmem)

# Type from BUILD_SHARED_LIBS. Visibility stays default: hiding it would mean
# annotating exported entities across twelve module interfaces, and a consumer
# compiles those interfaces itself either way. See docs/integration.md.
add_library(dgram)
add_library(dgram::dgram ALIAS dgram)

# No CXX_EXTENSIONS here: pinning it per target cannot be overridden by a
# consumer, and a consumer left at CMake's default then forks the `import std`
# BMI. The dialect is inherited; C++26 is a floor and stays pinned.
set_target_properties(dgram PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_MODULE_STD ON
    CXX_SCAN_FOR_MODULES ON
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

target_sources(dgram
  PUBLIC
    FILE_SET CXX_MODULES
      BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
      FILES
      src/dgram/error.cppm
      src/dgram/cmsg.cppm
      src/dgram/hash.cppm
      src/dgram/address.cppm
      src/dgram/socket.cppm
      src/dgram/feature.cppm
      src/dgram/metadata.cppm
      src/dgram/offload.cppm
      src/dgram/pacing.cppm
      src/dgram/demux.cppm
      src/dgram/timer.cppm
      src/dgram/batch.cppm
      src/dgram/dgram.cppm
)

# The C++26 floor as a usage requirement, not just a property on our own target:
# without it a consumer that sets no CMAKE_CXX_STANDARD fails at generate time
# with `does not include "cxx_std_20" (or newer)`.
target_compile_features(dgram PUBLIC cxx_std_26)

target_link_libraries(dgram PUBLIC libmem::libmem)

# The violation handler, always. Contracts are on by default in C++26, so a
# consumer that never sets an evaluation semantic still gets `enforce` and still
# needs this symbol. Linking it is not a compile flag, so it cannot fork a BMI;
# leaving it conditional only means an obscure link error for someone who did
# nothing wrong.
target_link_libraries(dgram PUBLIC stdc++exp)

add_library(dgram_flags INTERFACE)
target_compile_options(dgram_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wnrvo
)
# BUILD_INTERFACE: a PRIVATE link still reaches the export, bare, in
# IMPORTED_CXX_MODULES_LINK_LIBRARIES, and a consumer then compiles our module
# interfaces with our warnings rather than its own.
if(PROJECT_IS_TOP_LEVEL)
    target_link_libraries(dgram PRIVATE $<BUILD_LOCAL_INTERFACE:dgram_flags>)
endif()

# ---------------------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------------------
if(DGRAM_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(DGRAM_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/dgram")

    # cmake-lint predates FILE_SET and reads its DESTINATION as a duplicate
    # keyword; cmake rejects splitting the call ("not all of its interface file
    # sets are installed").
    # cmake-lint: disable=E1122
    install(TARGETS dgram
        EXPORT dgram-targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        # Module interfaces install as sources: the consumer builds their own BMIs.
        FILE_SET CXX_MODULES DESTINATION "${CMAKE_INSTALL_DATADIR}/dgram/modules"
    )

    install(EXPORT dgram-targets
        FILE dgram-targets.cmake
        NAMESPACE dgram::
        DESTINATION "${DGRAM_INSTALL_CMAKEDIR}"
        CXX_MODULES_DIRECTORY cxx-modules
    )

    # Consumers need this before their own project() call, so it ships separately.
    install(FILES cmake/enable_standard_modules.cmake
        DESTINATION "${DGRAM_INSTALL_CMAKEDIR}")

    # MIT asks for the notice to travel with copies; a package is a copy.
    install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DOCDIR}")

    configure_package_config_file(
        cmake/dgramConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/dgramConfig.cmake"
        INSTALL_DESTINATION "${DGRAM_INSTALL_CMAKEDIR}")

    # SameMinorVersion: pre-1.0, a minor bump is a break.
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/dgramConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/dgramConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/dgramConfigVersion.cmake"
        DESTINATION "${DGRAM_INSTALL_CMAKEDIR}")
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
