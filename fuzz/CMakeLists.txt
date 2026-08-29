# Fuzz harnesses.
#
# GCC ships no libFuzzer, and Clang cannot build this library at all: it supports
# neither expansion statements nor contracts. The coverage-guided path is
# therefore AFL++, whose compilers are GCC plugins.
#
# Select it by configuring the whole build with the AFL++ compiler, so the
# library is instrumented too and coverage reaches the parser:
#
#     cmake -S . -B build/afl -DCMAKE_CXX_COMPILER=afl-g++-fast -DDGRAM_BUILD_FUZZERS=ON
#
# `fuzz_driver.cpp` provides both entry points and picks between them on
# __AFL_COMPILER, so the target builds either way.

function(dgram_add_fuzzer name source)
    add_executable(${name} ${source} fuzz_driver.cpp)
    set_target_properties(${name} PROPERTIES CXX_STANDARD 26 CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
    target_link_libraries(${name} PRIVATE dgram dgram_flags)
endfunction()

dgram_add_fuzzer(fuzz_cmsg_parse fuzz_cmsg_parse.cpp)
dgram_add_fuzzer(fuzz_segments fuzz_segments.cpp)

if(CMAKE_CXX_COMPILER MATCHES "afl-")
    message(STATUS "[dgram] Fuzzing: AFL++ persistent mode, coverage-guided")
else()
    message(STATUS "[dgram] Fuzzing: standalone driver, not coverage-guided")
endif()

if(DGRAM_SANITIZER STREQUAL "none")
    message(WARNING "[dgram] Fuzzing without a sanitizer finds little; use -DDGRAM_SANITIZER=address+undefined")
endif()

# Under AFL++ the binary expects afl-fuzz to feed it, so it is not a ctest case.
# Under AFL++ the binaries expect afl-fuzz to feed them, so they are not ctest cases.
if(NOT CMAKE_CXX_COMPILER MATCHES "afl-")
    add_test(NAME Fuzz.CmsgParse COMMAND fuzz_cmsg_parse)
    add_test(NAME Fuzz.Segments COMMAND fuzz_segments)
    set_tests_properties(Fuzz.CmsgParse Fuzz.Segments PROPERTIES TIMEOUT 300)
endif()
