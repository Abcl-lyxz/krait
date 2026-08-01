#pragma once

#include "core/grid/cell.h"

#include <cstdint>
#include <vector>

namespace krait::core::vt {

// OSC 133 semantic shell-integration marks (T66). BITS, not an enum: A and B
// normally land on the SAME line — the prompt text and the command typed after
// it are one logical line — so an enum would make the second overwrite the
// first, and jump-to-prompt would lose every prompt the shell also marked B on.
inline constexpr std::uint8_t kMarkPromptStart = 1U << 0;  // OSC 133 ; A
inline constexpr std::uint8_t kMarkInputStart = 1U << 1;   // OSC 133 ; B
inline constexpr std::uint8_t kMarkOutputStart = 1U << 2;  // OSC 133 ; C
inline constexpr std::uint8_t kMarkCommandEnd = 1U << 3;   // OSC 133 ; D

// One visual row. A logical line is a maximal run of rows [i..j] where rows
// i+1..j carry wrappedFromPrev — wrap points are explicit from day one so
// reflow (M1) is a storage walk, not a rewrite (CLAUDE.md landmine).
struct Line {
    std::vector<Cell> cells;
    bool wrappedFromPrev = false;

    // Shell-integration marks for the LOGICAL line this row heads.
    //
    // Here rather than in a side table keyed by row number, and that is the
    // whole design: a row number is invalidated by every scroll, every
    // eviction and every reflow, so a mark stored beside the grid would point
    // at the wrong text the moment the user dragged the window. On the Line it
    // is carried by the same three code paths that already carry
    // wrappedFromPrev — Scrollback::push coalescing, reflow() re-splitting,
    // and eviction dropping the line and its mark together.
    std::uint8_t marks = 0;

    // Exit status from OSC 133 ; D ; <n>, written back onto the line that owns
    // the command (the one marked kMarkPromptStart). -1 = none: still running,
    // a bare `D`, or a status this parser refused (see osc.cpp).
    int exitCode = -1;

    explicit Line(int colCount = 0) : cells(static_cast<std::size_t>(colCount)) {}
};

}  // namespace krait::core::vt
