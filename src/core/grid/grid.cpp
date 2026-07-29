#include "core/grid/grid.h"

#include <algorithm>
#include <utility>

namespace krait::core::vt {

Grid::Grid(int rowCount, int colCount)
    : rows(std::max(1, rowCount)), cols(std::max(1, colCount)), damage(rows),
      m_screen(static_cast<std::size_t>(rows), Line(cols)) {}

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
        if (row + 1 >= rows) {
            scrollUp();
        } else {
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
    if (row + 1 >= rows) {
        scrollUp();
    } else {
        ++row;
    }
}

// ponytail: allocates one Line per scroll; recycle the popped buffer if the
// T13 bench shows it (per-line, not per-byte, so fine for now).
void Grid::scrollUp() {
    m_scrollback.push_back(std::move(m_screen.front()));
    if (m_scrollback.size() > kMaxScrollback) {
        m_scrollback.pop_front();
    }
    m_screen.erase(m_screen.begin());
    m_screen.emplace_back(cols);
    damage.markAll();
}

void Grid::resize(int newRows, int newCols) {
    // A minimized/0-height window must never produce a -1 cursor (UB).
    newRows = std::max(1, newRows);
    newCols = std::max(1, newCols);
    // Rows: shrink feeds the top into scrollback (content near the cursor
    // survives); growth appends blank rows at the bottom.
    while (rows > newRows) {
        m_scrollback.push_back(std::move(m_screen.front()));
        if (m_scrollback.size() > kMaxScrollback) {
            m_scrollback.pop_front();
        }
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
    damage.reset(rows);
    damage.markAll();
}

}  // namespace krait::core::vt
