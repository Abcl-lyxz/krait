#pragma once

#include "core/grid/line.h"

#include <span>
#include <vector>

namespace krait::core::vt {

// Result of rewrapping a screen. `lines` is the new visual layout; the cursor
// is reported in the NEW coordinates because a rewrap moves it — that is the
// whole point, and returning it separately keeps reflow() a pure function of
// its inputs.
struct ReflowResult {
    std::vector<Line> lines;
    int cursorRow = 0;
    int cursorCol = 0;
};

// Rewraps `rows` to `newCols`. A logical line is a maximal run of rows where
// every row after the first carries wrappedFromPrev (line.h); those runs are
// joined, their unwritten tail is trimmed, and the content is re-split at the
// new width.
//
// Two rules that are easy to get wrong and cost a rewrite if you do:
//
//   1. Only the UNWRITTEN tail is trimmed (Cell::ch == 0), never trailing
//      spaces. Krait can tell the two apart because erasure resets ch to 0
//      while a printed U+0020 stores 0x20 — so `echo "hi   "` keeps its
//      spaces while a 200-column blank tail does not become 200 cells of
//      content. Interior holes an application punched with CUP+EL are equally
//      preserved: the trim walks in from the end only.
//
//   2. A 2-column cluster is never split. Its trailing cell is already marked
//      kWideTrailing by the grid that wrote it, so this function does not need
//      to re-measure any widths — it moves the pair as a unit and lets a row
//      end one column short. Re-measuring here would be a second, divergent
//      source of truth for width.
//
// `cursorRow`/`cursorCol` locate the cursor in the OLD layout; pass a negative
// cursorRow when there is no cursor to track (an inactive buffer).
ReflowResult reflow(std::span<const Line> rows, int newCols, int cursorRow, int cursorCol);

}  // namespace krait::core::vt
