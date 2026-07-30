#pragma once

#include "render/shaper/shaper.h"

namespace krait::render {

// A rectangle in DEVICE pixels, relative to the terminal's top-left cell.
struct CellRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// Where the IME puts its candidate window, and where the composition is drawn
// (plan T29).
//
// rules/render.md: "composition string + candidate window positioning comes
// from the renderer's cell metrics — any metrics change re-runs the IME
// positioning tests (Thai + Japanese)". Hence a free function over FaceMetrics
// rather than something that reads the item: the metrics ARE the input, so a
// font or DPI change cannot move the glyphs without moving the candidate window
// with them, and a test needs no window to prove it.
//
// `col` may sit past the last column — a composition that grew past the right
// edge — and is clamped, because an IME handed a rectangle outside the widget
// puts its candidate list off-screen, or on the wrong monitor entirely.
CellRect cursorRect(const FaceMetrics& metrics, int row, int col, int cols, int rows);

// The span a preedit occupies: `cells` wide, starting at the cursor and clamped
// to the row. Used to draw the composition underline.
CellRect preeditRect(const FaceMetrics& metrics, int row, int col, int cells, int cols, int rows);

// How many CELLS a preedit of `columns` display columns occupies once clamped
// into a row starting at `col`. Separate from the rect because the caller also
// needs the count to decide how many glyphs to draw.
int preeditCells(int col, int columns, int cols);

}  // namespace krait::render
