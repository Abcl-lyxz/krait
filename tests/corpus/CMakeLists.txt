find_package(Catch2 3 CONFIG REQUIRED)

add_executable(krait-corpus-tests
    harness.cpp
)
target_link_libraries(krait-corpus-tests PRIVATE krait-core Catch2::Catch2WithMain)
target_compile_definitions(krait-corpus-tests
    PRIVATE KRAIT_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

include(Catch)
catch_discover_tests(krait-corpus-tests)
