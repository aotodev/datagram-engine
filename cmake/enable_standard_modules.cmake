include_guard(GLOBAL)

# ----------------------------------------------------------------------------
# MACRO: enable_experimental_std
# Call this BEFORE project() to set up the experimental UUIDs
# ----------------------------------------------------------------------------
macro(enable_experimental_std)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.4.0")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")

    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3.0")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")

    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")

    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.0")
        # 4.0.0 - 4.0.2 are the only ones with a different uuid in the series 3.31.8 - 4.2.4
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")

    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31.8")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")

    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30.0")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")

    else()
        message(WARNING "CMake ${CMAKE_VERSION} does not support experimental 'import std;'")
    endif()
endmacro()
