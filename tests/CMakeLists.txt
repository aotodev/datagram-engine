include(FetchContent)
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        SYSTEM
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# Compiling is the test: pure static_assert over the compile-time layer.
add_library(dgram_compile_checks OBJECT compile_checks.cpp)
target_link_libraries(dgram_compile_checks PRIVATE dgram dgram_flags)
set_target_properties(dgram_compile_checks PROPERTIES CXX_STANDARD 26 CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)

add_executable(dgram_tests loopback_tests.cpp metadata_tests.cpp malformed_control_tests.cpp concurrency_tests.cpp)
target_link_libraries(dgram_tests PRIVATE dgram dgram_flags GTest::gtest_main)
find_package(Threads REQUIRED)
target_link_libraries(dgram_tests PRIVATE Threads::Threads)
set_target_properties(dgram_tests PROPERTIES CXX_STANDARD 26 CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)

include(GoogleTest)
gtest_discover_tests(dgram_tests)
