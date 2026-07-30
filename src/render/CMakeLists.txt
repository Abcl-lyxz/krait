# krait-shaper — HarfBuzz + FreeType text shaping (plan T23).
#
# Deliberately Qt-free: it links only krait-core, HarfBuzz and FreeType, so the
# plain Catch2 test binary can exercise real shaping without a QGuiApplication.
# The QRhi renderer (T25) is the layer that adds Qt on top of this.
find_package(harfbuzz CONFIG REQUIRED)
find_package(freetype CONFIG REQUIRED)

add_library(krait-shaper STATIC
    atlas/glyph_atlas.cpp
    frame_builder.cpp
    shaper/fontdb.cpp
    shaper/run_splitter.cpp
    shaper/shape_pool.cpp
    shaper/shaper.cpp
)
target_include_directories(krait-shaper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# PUBLIC krait-core: Run carries core::vt::Attr and splitRow takes core Cells.
# PRIVATE for the shaping engines — a consumer sees ShapedRun, never hb_font_t
# (shaper.h forward-declares the handles for exactly this reason).
#
# dwrite: T24 uses DirectWrite for font DISCOVERY only — which file holds a face
# that covers this text — and FreeType still does every rasterisation (ADR-0001).
# PRIVATE keeps dwrite_2.h out of every consumer's include path.
target_link_libraries(krait-shaper PUBLIC krait-core PRIVATE harfbuzz::harfbuzz freetype dwrite)
