find_package(Catch2 3 CONFIG REQUIRED)

add_executable(krait-core-tests
    grid_test.cpp
    reflow_test.cpp
    session_test.cpp
    smoke_test.cpp
    width_test.cpp
)
target_link_libraries(krait-core-tests PRIVATE krait-core Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(krait-core-tests)
