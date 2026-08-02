cmake_minimum_required(VERSION 3.30)

project(krait VERSION 0.0.1 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Krait targets MSVC on Windows (see docs/decisions/).")
endif()

# C++23 preview until VS finishes /std:c++23 (rules/cpp.md); warnings are errors.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl (fuzz preset): same language level, its own flag spellings.
    # NOTE: untested until clang-cl is installed on the dev machine.
    add_compile_options(/clang:-std=c++23 /W4 /WX /utf-8)
else()
    add_compile_options(/std:c++23preview /W4 /WX /utf-8 /permissive- /Zc:preprocessor)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

option(KRAIT_FUZZ "Build the libFuzzer target instead of the test suite" OFF)

if(KRAIT_FUZZ)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Coverage instrumentation for everything the fuzzer executes.
        add_compile_options(-fsanitize=fuzzer-no-link,address)
    else()
        # MSVC: sanitizer+coverage must match across the whole build or the
        # ASan container annotations mismatch at link (LNK2038).
        add_compile_options(/fsanitize=address /fsanitize=fuzzer)
    endif()
    add_subdirectory(src/core)
    add_subdirectory(tests/fuzz)
else()
    add_subdirectory(src/core)
    add_subdirectory(src/render)
    # Before tests/unit: the keymap table is unit-tested, and a target cannot be
    # linked before it is defined. The REST of src/app stays last — it drags in
    # Qt Quick, which the test binaries have no business linking.
    add_subdirectory(src/app/input)
    add_subdirectory(src/app/crash)
    add_subdirectory(src/app/settings)
    add_subdirectory(src/app/theme)
    add_subdirectory(src/app/session)
    # krait-net before tests/unit for the same reason as krait-input: the
    # IBackend contract suite links it, and a target cannot be linked before it
    # is defined.
    add_subdirectory(src/net)
    add_subdirectory(tests/unit)
    add_subdirectory(tests/corpus)
    add_subdirectory(src/app)

    # LAST, and it has to be: CPack packages the install() rules that exist
    # when include(CPack) runs, so anything added after this is silently
    # missing from every artefact.
    include(packaging/packaging.cmake)
endif()
