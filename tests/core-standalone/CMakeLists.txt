# Zero-dependency proof (rules/vt-core.md): configures krait-core as its own
# top-level project with no vcpkg toolchain and no Qt anywhere on the path.
# Breaking this build is breaking the build.
cmake_minimum_required(VERSION 3.30)

project(krait-core-standalone LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Krait targets MSVC on Windows.")
endif()
add_compile_options(/std:c++23preview /W4 /WX /utf-8 /permissive- /Zc:preprocessor)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core core)
