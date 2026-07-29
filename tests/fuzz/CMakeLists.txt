# libFuzzer target (ADR-0010). Only configured when KRAIT_FUZZ=ON — the
# fuzz presets do that; the dev preset never sees this directory.
add_executable(parser-fuzz parser_fuzz.cpp)
target_link_libraries(parser-fuzz PRIVATE krait-core)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Primary path: clang-cl. Library code gets coverage instrumentation via
    # the root flags; the target itself links the fuzzer runtime.
    target_compile_options(parser-fuzz PRIVATE -fsanitize=fuzzer,address)
    target_link_options(parser-fuzz PRIVATE -fsanitize=fuzzer,address)
else()
    # Fallback: MSVC's experimental libFuzzer (ADR-0010 informational tier).
    # Its runtime expects ASan to be present (separate flags per MSVC docs).
    target_compile_options(parser-fuzz PRIVATE /fsanitize=fuzzer /fsanitize=address)
    target_link_options(parser-fuzz PRIVATE /fsanitize=fuzzer /fsanitize=address)
endif()
