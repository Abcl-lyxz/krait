cmake_minimum_required(VERSION 3.30)

project(krait VERSION 0.0.1 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Krait targets MSVC on Windows (see docs/decisions/).")
endif()

# C++23 preview until VS finishes /std:c++23 (rules/cpp.md); warnings are errors.
add_compile_options(/std:c++23preview /W4 /WX /utf-8 /permissive- /Zc:preprocessor)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(tests/unit)

# T11 adds: add_subdirectory(src/render) + add_subdirectory(src/app)
