#pragma once

#include "core/grid/line.h"

#include <cstddef>
#include <deque>
#include <vector>

namespace krait::core::vt {

// History above the screen, stored as LOGICAL lines — the unwrapped text an
// application printed, not the visual rows it happened to occupy at whatever
// width it was printed at. That distinction is the CLAUDE.md landmine: history
// kept as fixed-width rows cannot be rewrapped later without a rewrite, so the
// ring coalesces continuation rows on the way in and re-splits them on the way
// out (reflow.h), at whatever width is current when someone looks.
//
// Two independent caps, because a cap counted only in LINES is not a memory
// bound: one logical line can be arbitrarily long, so a stream that never emits
// a newline would grow the ring forever without ever adding a line to it. That
// is a plain denial of service from remote bytes (rules/net.md), so cells are
// bounded too, and one line cannot exceed the cell budget on its own.
class Scrollback {
  public:
    static constexpr std::size_t kDefaultMaxLines = 10'000;
    // ~80 MB at 20 bytes per Cell — the same order as the line cap at a typical
    // width, so neither bound is the one that always fires.
    static constexpr std::size_t kDefaultMaxCells = 4'000'000;

    // Appends a visual row retired off the top of the screen. A row carrying
    // wrappedFromPrev EXTENDS the last logical line rather than starting a new
    // one; that flag is the wrap point Line has recorded since T8.
    void push(Line&& row);

    std::size_t lineCount() const { return m_lines.size(); }

    std::size_t cellCount() const { return m_cells; }

    bool empty() const { return m_lines.empty(); }

    // Logical line i, oldest first.
    const Line& lineAt(std::size_t i) const { return m_lines[i]; }

    // The last `count` VISUAL rows of history, wrapped at `cols`. This is what
    // a viewport scrolled up into history draws.
    std::vector<Line> tailRows(int cols, std::size_t count) const;

    void setCaps(std::size_t maxLines, std::size_t maxCells);
    void clear();

  private:
    void evict();

    std::deque<Line> m_lines;
    std::size_t m_cells = 0;
    std::size_t m_maxLines = kDefaultMaxLines;
    std::size_t m_maxCells = kDefaultMaxCells;
};

}  // namespace krait::core::vt
