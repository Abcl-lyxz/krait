#pragma once

#include "core/grid/cell.h"

#include <cstdint>
#include <string>
#include <vector>

namespace krait::render {

// A Unicode script, as HarfBuzz's 4-byte ISO 15924 tag. Stored as a plain
// integer so this header stays free of HarfBuzz types — src/app and the tests
// include it, and only shaper.cpp / run_splitter.cpp should need hb.h.
using ScriptTag = std::uint32_t;

// The attribute bits that change which GLYPHS a run produces, as opposed to
// how they are coloured. Only these belong in the shaped-run cache key: fg/bg
// and the underline style are draw-time state, and folding them in would miss
// the cache on every recolour of identical text.
constexpr std::uint16_t shapingBits(const core::vt::Attr& attr) {
    return attr.flags & (core::vt::Attr::kBold | core::vt::Attr::kItalic);
}

// One grapheme cluster inside a run. The grid already segmented it (T19/T20) —
// `len` codepoints of the run's text belong to this cluster and nothing here
// re-runs segmentation.
struct ClusterRef {
    int col = 0;               // column of the cluster's FIRST cell
    std::uint8_t cells = 1;    // 1, or 2 for a wide cluster
    std::uint8_t len = 1;      // codepoints in the run's text
    std::uint32_t offset = 0;  // index of the first codepoint in the run's text
};

// A maximal span of one row that can go to HarfBuzz as a single buffer: same
// shaping attributes, same script, no gaps. See run_splitter.h for the rules.
struct Run {
    int row = 0;
    int col = 0;  // first column; equals clusters.front().col
    std::u32string text;
    std::vector<ClusterRef> clusters;
    // Only the shaping-relevant bits, NOT the full Attr. Deliberate: a Run
    // deliberately spans a colour change, so storing one Attr here would invite
    // the draw to paint the whole run in the first cell's colour. Colours are
    // per-cell state and the renderer reads them by column.
    std::uint16_t shaping = 0;
    ScriptTag script = 0;
    bool rightToLeft = false;
};

// One positioned glyph. Units are 26.6 fixed point (pixels * 64), which is what
// HarfBuzz reports for a font created over an FT_Face whose size was set in
// pixels — the same fixed point FreeType uses, kept unrounded so subpixel
// positioning stays available to the atlas.
struct ShapedGlyph {
    std::uint32_t glyphId = 0;
    std::int32_t xAdvance = 0;
    std::int32_t yAdvance = 0;
    std::int32_t xOffset = 0;
    std::int32_t yOffset = 0;
    std::uint32_t cluster = 0;  // index into the Run's clusters vector
};

// The result of shaping one Run with one face.
struct ShapedRun {
    std::vector<ShapedGlyph> glyphs;
    std::uint32_t faceId = 0;
    // True when any glyphId came back 0 (.notdef): the face does not cover this
    // text and T24's fallback chain has to re-shape it. Recorded at shape time
    // because scanning for it later would mean walking every glyph again.
    bool missingGlyphs = false;
};

}  // namespace krait::render
