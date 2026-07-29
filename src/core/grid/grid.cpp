#include "core/grid/grid.h"

#include <algorithm>
#include <utility>

namespace krait::core::vt {

Grid::Grid(int rowCount, int colCount)
    : rows(std::max(1, rowCount)), cols(std::max(1, colCount)), damage(rows),
      scrollBottom(rows - 1), m_screen(static_cast<std::size_t>(rows), Line(cols)) {}

Cell& Grid::cellAt(int r, int c) {
    return m_screen[static_cast<std::size_t>(r)].cells[static_cast<std::size_t>(c)];
}

const Cell& Grid::cellAt(int r, int c) const {
    return m_screen[static_cast<std::size_t>(r)].cells[static_cast<std::size_t>(c)];
}

Line& Grid::lineAt(int r) {
    return m_screen[static_cast<std::size_t>(r)];
}

const Line& Grid::lineAt(int r) const {
    return m_screen[static_cast<std::size_t>(r)];
}

void Grid::putChar(char32_t ch) {
    if (pendingWrap) {
        pendingWrap = false;
        // Wrapping at the bottom MARGIN scrolls the region, not the screen:
        // text inside a region must stay inside it. Below the region the
        // cursor just walks down until it runs out of screen.
        if (row == scrollBottom) {
            scrollRegionUp(1);
        } else if (row + 1 < rows) {
            ++row;
        }
        lineAt(row).wrappedFromPrev = true;  // the wrap point, recorded
        col = 0;
    }
    cellAt(row, col) = {ch, pen};
    damage.mark(row, col, col);
    if (col + 1 < cols) {
        ++col;
    } else {
        pendingWrap = true;  // DEC deferred wrap: stay on the last column
    }
}

void Grid::linefeed() {
    pendingWrap = false;
    // At the bottom margin LF scrolls the region. A cursor already BELOW the
    // region is outside it, so it may not scroll anything — it walks down to
    // the last screen row and stops (DEC: no scrolling outside the margins).
    if (row == scrollBottom) {
        scrollRegionUp(1);
    } else if (row + 1 < rows) {
        ++row;
    }
}

void Grid::pushToScrollback(Line&& line) {
    m_scrollback.push_back(std::move(line));
    if (m_scrollback.size() > kMaxScrollback) {
        m_scrollback.pop_front();
    }
}

// ponytail: rotates whole Line objects, so it allocates at most one Line per
// call rather than per row. Per-line, not per-byte — fine until a bench says
// otherwise.
void Grid::scrollRegionUp(int n) {
    const int height = scrollBottom - scrollTop + 1;
    if (n <= 0 || height <= 0) {
        return;
    }
    n = std::min(n, height);
    const bool toHistory = fullScreenRegion();
    for (int k = 0; k < n; ++k) {
        Line& top = m_screen[static_cast<std::size_t>(scrollTop)];
        if (toHistory) {
            pushToScrollback(std::move(top));
        }
        // Rotate the region up by one, then blank the bottom row. The blank
        // is a fresh Line so it carries no attributes (DEC: "blank lines with
        // no visual character attributes") and no wrap flag.
        for (int r = scrollTop; r < scrollBottom; ++r) {
            m_screen[static_cast<std::size_t>(r)] =
                std::move(m_screen[static_cast<std::size_t>(r + 1)]);
        }
        m_screen[static_cast<std::size_t>(scrollBottom)] = Line(cols);
    }
    // The row now at the top of the region is no longer a wrap continuation of
    // whatever scrolled away above it.
    m_screen[static_cast<std::size_t>(scrollTop)].wrappedFromPrev = false;
    damage.markAll();
}

void Grid::scrollRegionDown(int n) {
    const int height = scrollBottom - scrollTop + 1;
    if (n <= 0 || height <= 0) {
        return;
    }
    n = std::min(n, height);
    for (int k = 0; k < n; ++k) {
        // Lines pushed off the BOTTOM are always lost — scrollback is history
        // above the screen, never below it.
        for (int r = scrollBottom; r > scrollTop; --r) {
            m_screen[static_cast<std::size_t>(r)] =
                std::move(m_screen[static_cast<std::size_t>(r - 1)]);
        }
        m_screen[static_cast<std::size_t>(scrollTop)] = Line(cols);
    }
    // A blank line was inserted at the top of the region, so the row below the
    // inserted block must not claim to continue it.
    if (scrollTop + n <= scrollBottom) {
        m_screen[static_cast<std::size_t>(scrollTop + n)].wrappedFromPrev = false;
    }
    damage.markAll();
}

void Grid::resize(int newRows, int newCols) {
    // A minimized/0-height window must never produce a -1 cursor (UB).
    newRows = std::max(1, newRows);
    newCols = std::max(1, newCols);
    // Rows: shrink feeds the top into scrollback (content near the cursor
    // survives); growth appends blank rows at the bottom.
    while (rows > newRows) {
        pushToScrollback(std::move(m_screen.front()));
        m_screen.erase(m_screen.begin());
        --rows;
        row = std::max(0, row - 1);
    }
    while (rows < newRows) {
        m_screen.emplace_back(cols);
        ++rows;
    }
    // Columns: truncate/pad in place. ponytail: no rewrap — M1 reflow walks
    // the wrap flags this class has recorded since day one.
    if (cols != newCols) {
        cols = newCols;
        for (Line& line : m_screen) {
            line.cells.resize(static_cast<std::size_t>(newCols));
        }
    }
    row = std::min(row, rows - 1);
    col = std::min(col, cols - 1);
    pendingWrap = false;
    // A resize can leave the margins describing rows that no longer exist. An
    // out-of-range scrollBottom would make scrollRegionUp index past the
    // screen, so re-clamp rather than trusting the old values. xterm likewise
    // resets the region on resize.
    scrollTop = 0;
    scrollBottom = rows - 1;
    damage.reset(rows);
    damage.markAll();
}

}  // namespace krait::core::vt
