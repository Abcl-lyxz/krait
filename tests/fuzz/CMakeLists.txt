# libFuzzer targets (ADR-0010). Only configured when KRAIT_FUZZ=ON — the
# fuzz presets do that; the dev preset never sees this directory.
add_executable(parser-fuzz parser_fuzz.cpp)
target_link_libraries(parser-fuzz PRIVATE krait-core)

# T54. telnet_negotiation.cpp is compiled straight in rather than linked
# through krait-net: the negotiator is deliberately Qt-free, and linking the
# library would drag Qt6::Core and Qt6::Network into a sanitiser build for no
# reason. The same property that makes it unit-testable makes this cheap.
add_executable(telnet-fuzz
    telnet_fuzz.cpp
    ../../src/net/telnet/telnet_negotiation.cpp
)
target_include_directories(telnet-fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net)

# One loop rather than a copy of the flags per target: a third fuzz target
# added with the sanitiser flags forgotten still builds, still runs, and finds
# nothing — which reads exactly like a clean fuzz run.
foreach(krait_fuzz_target parser-fuzz telnet-fuzz)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Primary path: clang-cl. Library code gets coverage instrumentation via
        # the root flags; the target itself links the fuzzer runtime.
        target_compile_options(${krait_fuzz_target} PRIVATE -fsanitize=fuzzer,address)
        target_link_options(${krait_fuzz_target} PRIVATE -fsanitize=fuzzer,address)
    else()
        # Fallback: MSVC's experimental libFuzzer (ADR-0010 informational tier).
        # Its runtime expects ASan to be present (separate flags per MSVC docs).
        target_compile_options(${krait_fuzz_target} PRIVATE /fsanitize=fuzzer /fsanitize=address)
        target_link_options(${krait_fuzz_target} PRIVATE /fsanitize=fuzzer /fsanitize=address)
    endif()
endforeach()
