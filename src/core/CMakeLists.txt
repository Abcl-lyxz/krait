# krait-core — the sacred zone (rules/vt-core.md): STL + utf8proc only.
# Zero Qt/network/render deps, proven by tests/core-standalone.
add_library(krait-core STATIC
    caps/caps.cpp
    grid/grid.cpp
    parser/csi_cursor.cpp
    parser/machine.cpp
    parser/sgr.cpp
    terminal/session.cpp
    unicode/utf8.cpp
    version.cpp
)
target_include_directories(krait-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
