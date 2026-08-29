add_executable(echo echo.cpp)
target_link_libraries(echo PRIVATE dgram dgram_flags)
set_target_properties(echo PROPERTIES CXX_STANDARD 26 CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
