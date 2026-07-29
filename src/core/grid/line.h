#pragma once

#include "core/grid/cell.h"

#include <vector>

namespace krait::core::vt {

// One visual row. A logical line is a maximal run of rows [i..j] where rows
// i+1..j carry wrappedFromPrev — wrap points are explicit from day one so
// reflow (M1) is a storage walk, not a rewrite (CLAUDE.md landmine).
struct Line {
    std::vector<Cell> cells;
    bool wrappedFromPrev = false;

    explicit Line(int colCount = 0) : cells(static_cast<std::size_t>(colCount)) {}
};

}  // namespace krait::core::vt
