#pragma once

#include <QImage>
#include <QStringList>

namespace krait::render {

// ASCII 0x20..0x7E rasterized into a single-row R8 atlas of uniform cells
// (95 cells wide). Spike-only: the real shaper-fed atlas lands post-M0.
struct GlyphAtlas {
    QImage image;  // Format_Grayscale8, tightly packed rows
    int cellWidth = 0;
    int cellHeight = 0;
    int baseline = 0;  // pixels from cell top to the glyph baseline

    bool valid() const { return !image.isNull(); }
};

// Rasterizes with FreeType at pixelHeight using the first font path that
// exists. Returns an invalid atlas if no font loads.
GlyphAtlas buildAsciiAtlas(const QStringList& fontPaths, int pixelHeight);

}  // namespace krait::render
