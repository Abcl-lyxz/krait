find_package(Catch2 3 CONFIG REQUIRED)

add_executable(krait-core-tests
    atlas_test.cpp
    grid_test.cpp
    modes_test.cpp
    reflow_test.cpp
    scrollback_test.cpp
    fontdb_test.cpp
    frame_builder_test.cpp
    session_test.cpp
    shaper_test.cpp
    smoke_test.cpp
    width_test.cpp
)
# krait-shaper brings krait-core with it (PUBLIC), and stays Qt-free so real
# HarfBuzz shaping is testable here without a QGuiApplication.
target_link_libraries(krait-core-tests PRIVATE krait-core krait-shaper Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(krait-core-tests)
