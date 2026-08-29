include_guard(GLOBAL)

# Sanitizers and hardening are applied at directory scope, before any dependency
# is configured. `import std;` builds its BMI from the first target that needs
# it, so a per-target flag forks the BMI and every importer fails with a CRC
# mismatch. _GLIBCXX_ASSERTIONS has the same requirement.

set(DGRAM_SANITIZER "none" CACHE STRING "Sanitizer: none, address, undefined, address+undefined, thread")
set_property(CACHE DGRAM_SANITIZER PROPERTY STRINGS none address undefined address+undefined thread)

option(DGRAM_HARDENED "libstdc++ assertions, fortified sources, stack protection" OFF)

# ---------------------------------------------------------------------------
# dgram_apply_hardening: call before FetchContent_MakeAvailable.
# ---------------------------------------------------------------------------
macro(dgram_apply_hardening)
    set(_dgram_san_flags "")

    if(DGRAM_SANITIZER STREQUAL "address")
        set(_dgram_san_flags -fsanitize=address)
    elseif(DGRAM_SANITIZER STREQUAL "undefined")
        set(_dgram_san_flags -fsanitize=undefined)
    elseif(DGRAM_SANITIZER STREQUAL "address+undefined")
        set(_dgram_san_flags -fsanitize=address,undefined)
    elseif(DGRAM_SANITIZER STREQUAL "thread")
        set(_dgram_san_flags -fsanitize=thread)
    elseif(NOT DGRAM_SANITIZER STREQUAL "none")
        message(FATAL_ERROR "DGRAM_SANITIZER must be none, address, undefined, address+undefined or thread")
    endif()

    # We own instrumentation, so libmem must not add its own. Its USE_SANITIZERS
    # defaults ON and applies flags per-config through an interface target, which
    # reaches every real target but not CMake's synthesised `import std` one. In
    # a Debug build that asymmetry alone is a std BMI CRC mismatch. Our
    # directory-wide flags already cover libmem's sources.
    set(USE_SANITIZERS OFF CACHE BOOL "" FORCE)
    set(THREAD_SANITIZER OFF CACHE BOOL "" FORCE)

    if(_dgram_san_flags)
        add_compile_options(${_dgram_san_flags} -fno-omit-frame-pointer -g)
        add_link_options(${_dgram_san_flags})
        message(STATUS "[dgram] Sanitizer: ${DGRAM_SANITIZER}")
    endif()

    if(DGRAM_HARDENED)
        # Bounds and precondition checks inside libstdc++: turns a bad span index
        # or subspan into a trap instead of a silent read.
        #
        # Passed as an option, not add_compile_definitions: CMake's synthesised
        # `import std` target inherits COMPILE_OPTIONS from the directory but not
        # COMPILE_DEFINITIONS, so a define reaches every target except the std
        # BMI and every importer then fails with a CRC mismatch.
        add_compile_options(-D_GLIBCXX_ASSERTIONS)
        add_compile_options(-fstack-protector-strong -fstack-clash-protection -ftrivial-auto-var-init=pattern)

        # _FORTIFY_SOURCE is a no-op without optimisation and warns about it.
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_options(-D_FORTIFY_SOURCE=3)
        endif()
        message(STATUS "[dgram] Hardened build")
    endif()
endmacro()
