# krait-core — the sacred zone (rules/vt-core.md): STL + utf8proc only.
# Zero Qt/network/render deps, proven by tests/core-standalone.
#
# utf8proc is the ONE permitted external (ADR-0003: grapheme segmentation, and
# from T19 also per-codepoint width — it ships UCD 17.0.0, so we generate no
# tables of our own; see docs/research/t19-width-findings.md). Use the
# `utf8proc` config, NOT `unofficial-utf8proc`, which is deprecated upstream.
find_package(utf8proc CONFIG REQUIRED)

add_library(krait-core STATIC
    caps/caps.cpp
    grid/cluster_pool.cpp
    grid/grid.cpp
    grid/reflow.cpp
    grid/scrollback.cpp
    parser/csi_cursor.cpp
    parser/csi_mode.cpp
    parser/csi_scroll.cpp
    parser/machine.cpp
    parser/sgr.cpp
    terminal/session.cpp
    unicode/utf8.cpp
    unicode/width.cpp
    version.cpp
)
target_include_directories(krait-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# PRIVATE: utf8proc is an implementation detail of width.cpp. Consumers of
# krait-core get grapheme clusters through our own types, never utf8proc's —
# that keeps the one external dependency from leaking into src/app and src/net.
target_link_libraries(krait-core PRIVATE utf8proc::utf8proc)
