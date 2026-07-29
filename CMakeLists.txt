cmake_minimum_required(VERSION 3.30)

project(krait VERSION 0.0.1 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Krait targets MSVC on Windows (see docs/decisions/).")
endif()

# C++23 preview until VS finishes /std:c++23 (rules/cpp.md); warnings are errors.
add_compile_options(/std:c++23preview /W4 /WX /utf-8 /permissive- /Zc:preprocessor)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# Targets land per docs/plan/02-m0-tasks.md:
#   T3  add_subdirectory(src/core) + tests/
#   T11 add_subdirectory(src/render) + add_subdirectory(src/app)
