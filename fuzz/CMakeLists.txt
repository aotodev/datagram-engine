# Fuzz harnesses. GCC ships no libFuzzer and Clang cannot build this library
# (it supports neither expansion statements nor contracts), so the coverage-guided
# path is AFL++, whose compilers are GCC plugins. Without AFL++ the same harness
# links against a standalone driver instead.

find_program(AFL_CXX NAMES afl-g++-fast afl-g++ afl-c++)

add_executable(fuzz_cmsg_parse fuzz_cmsg_parse.cpp)
set_target_properties(fuzz_cmsg_parse PROPERTIES CXX_STANDARD 26 CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
target_link_libraries(fuzz_cmsg_parse PRIVATE dgram dgram_flags)

if(AFL_CXX)
    message(STATUS "[dgram] Fuzzing: AFL++ (${AFL_CXX}), coverage-guided")
    set_target_properties(fuzz_cmsg_parse PROPERTIES CXX_COMPILER_LAUNCHER "")
else()
    message(STATUS "[dgram] Fuzzing: standalone driver, not coverage-guided (AFL++ not found)")
    target_sources(fuzz_cmsg_parse PRIVATE fuzz_driver.cpp)
endif()

if(DGRAM_SANITIZER STREQUAL "none")
    message(WARNING "[dgram] Fuzzing without a sanitizer finds little; configure -DDGRAM_SANITIZER=address+undefined")
endif()

add_test(NAME Fuzz.CmsgParse COMMAND fuzz_cmsg_parse)
set_tests_properties(Fuzz.CmsgParse PROPERTIES TIMEOUT 300)
