#include "core/grid/grid.h"

#include <algorithm>
#include <utility>

namespace krait::core::vt {

Grid::Grid(int rowCount, int colCount)
    : rows(std::max(1, rowCount)), cols(std::max(1, colCount)), damage(rows),
      scrollBottom(rows - 1), m_screen(static_cast<std::size_t>(rows), Line(cols)) {}

void Grid::cursorSet(int r, int c) {
    // xterm cursor.c: with ORIGIN set the row is offset by the top margin and
    // clamped to the BOTTOM MARGIN; without it the clamp is the page. The only
    // low clamp is 0 — do NOT assume the result is at or below scrollTop. A
    // caller may legitimately pass a NEGATIVE relative row and land above the
    // top margin with origin mode still on: restoreCursor() does exactly that
    // when the margins moved while the alternate screen was up, which is why
    // caps.cpp's CPR has to clamp its subtraction.
    const int maxRow = originMode ? scrollBottom : rows - 1;
    if (originMode) {
        r += scrollTop;
    }
    row = std::clamp(r, 0, maxRow);
    col = std::clamp(c, 0, cols - 1);
    pendingWrap = false;
}

void Grid::eraseScreen() {
    // ponytail: blanks with fresh default cells, not the current pen. BCE is
    // one undecided question across ED/EL/IL/DL/SU/SD (see conformance.md) and
    // this stays consistent with scrollRegion*'s DEC-cited attribute-free
    // blanks; terminalguide says xterm's 1049 clear DOES use the pen, so this
    // flips with the rest of BCE, not on its own.
    for (Line& line : m_screen) {
        line = Line(cols);
    }
    damage.markAll();
}

void Grid::useAlternateScreen(bool on) {
    if (on == m_onAlt) {
        return;  // idempotent: xterm's 1049 handler is a no-op when already there
    }
    // Allocated on first use. No shape check beyond this: resize() reshapes the
    // inactive buffer too, so once it exists it is always rows x cols.
    if (m_altScreen.empty()) {
        m_altScreen.assign(static_cast<std::size_t>(rows), Line(cols));
    }
    m_screen.swap(m_altScreen);
    m_onAlt = on;
    damage.markAll();
}

void Grid::saveCursor() {
    m_saved[m_onAlt ? 1 : 0] = {row, col, pen, originMode, pendingWrap, g1Invoked};
}

void Grid::restoreCursor() {
    const SavedCursor sc = m_saved[m_onAlt ? 1 : 0];
    pen = sc.pen;
    originMode = sc.originMode;
    g1Invoked = sc.g1Invoked;
    // Saved rows are ABSOLUTE, but cursorSet() re-adds the top margin when
    // origin mode is on — so take it back out first (xterm CursorRestore does
    // exactly this: CursorSet(screen, sc->row - screen->top_marg, ...)).
    cursorSet(originMode ? sc.row - scrollTop : sc.row, sc.col);
    pendingWrap = sc.pendingWrap;  // cursorSet() just cleared it; the slot wins
}

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
    const bool toHistory = capturesScrollback();
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
    // xterm's ScreenResize reallocates the INACTIVE buffer too, so the normal
    // screen survives a resize taken while a full-screen app owns the alternate
    // one. Shrinking drops rows off the TOP, not the bottom — xterm's
    // Reallocate: "If the screen shrinks, remove lines off the top of the
    // buffer", under the default SouthWest gravity, for both buffers. Getting
    // this end wrong would destroy the shell prompt every time a window shrank
    // inside vim. ponytail: no reflow and no scrollback push, same as above.
    if (!m_altScreen.empty()) {
        while (m_altScreen.size() > static_cast<std::size_t>(rows)) {
            m_altScreen.erase(m_altScreen.begin());
        }
        m_altScreen.resize(static_cast<std::size_t>(rows), Line(newCols));
        for (Line& line : m_altScreen) {
            line.cells.resize(static_cast<std::size_t>(newCols));
        }
    }
    row = std::min(row, rows - 1);
    col = std::min(col, cols - 1);
    pendingWrap = false;
    // A resize can leave the margins describing rows that no longer exist. An
    // out-of-range scrollBottom would make scrollRegionUp index past the
    // screen, so re-clamp rather than trusting the old values. xterm likewise
    // resets the region on resize — and in the same breath clears DECOM
    // (screen.c: resetMargins(xw) then UIntClr(*flags, ORIGIN)), which it must,
    // since origin mode without its margins would address a stale region.
    scrollTop = 0;
    scrollBottom = rows - 1;
    originMode = false;
    damage.reset(rows);
    damage.markAll();
}

}  // namespace krait::core::vt
