# Zero-dependency proof (rules/vt-core.md): configures krait-core as its own
# top-level project with no Qt anywhere on the path. What this actually proves —
# and the only thing worth proving — is that src/core/ pulls in no Qt, no
# networking and no rendering. utf8proc and the STL are the permitted externals
# (rules/vt-core.md, ADR-0003), so from T19 this build needs utf8proc too.
#
# Point it at utf8proc and NOTHING else:
#   -Dutf8proc_DIR=<repo>/build/dev/vcpkg_installed/x64-windows/share/utf8proc
# A broad -DCMAKE_PREFIX_PATH over vcpkg_installed also works but is weaker —
# that tree holds fmt, freetype and Catch2 too, so a stray find_package(fmt) in
# src/core would resolve and this guard would pass green. CI uses utf8proc_DIR
# for that reason, and additionally blanks QT_ROOT / Qt6_DIR / CMAKE_PREFIX_PATH
# so a find_package(Qt6) cannot resolve from the Qt action's exports.
#
# Breaking this build is breaking the build.
cmake_minimum_required(VERSION 3.30)

project(krait-core-standalone LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Krait targets MSVC on Windows.")
endif()
add_compile_options(/std:c++23preview /W4 /WX /utf-8 /permissive- /Zc:preprocessor)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core core)
