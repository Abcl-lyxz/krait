#include "core/grid/grid.h"

#include "core/grid/reflow.h"

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace krait::core::vt {

namespace {

bool isBlankLine(const Line& line) {
    return std::all_of(line.cells.begin(), line.cells.end(),
                       [](const Cell& cell) { return cell.ch == 0; });
}

}  // namespace

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
    resetClusterAnchor();  // the cell a following mark would join is gone
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
    resetClusterAnchor();  // the anchored cell belongs to the other buffer now
    // A full-screen application owns the whole viewport; leaving it scrolled up
    // would paint shell history over vim.
    m_viewOffset = 0;
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

void Grid::resetClusterAnchor() {
    m_clusterRow = -1;
    m_clusterCol = -1;
    m_clusterLen = 0;
    m_clusterCh = 0;
}

void Grid::wrapToNextRow() {
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

void Grid::breakWidePairs(int r, int first, int last) {
    // A 2-column cluster occupies two cells that only mean anything together.
    // Writing into either half strands the other, and a stranded half is not
    // cosmetic: two adjacent trailings make the renderer walk left looking for
    // a lead and find another trailing, and a lead with no trailing draws a
    // wide glyph over a cell somebody else owns. Found by the fuzzer —
    // `U+FFFD`, a wide CJK char, CR, another wide char — where the second
    // write landed its own trailing on the first pair's LEAD and left that
    // pair's trailing beside it.
    //
    // Blanks with default cells, matching eraseScreen: BCE is one undecided
    // question across the whole grid and this is not the place to answer it.
    if (first > 0 && cellAt(r, first).ch == kWideTrailing) {
        cellAt(r, first - 1) = Cell{};  // its lead loses the right half
        damage.mark(r, first - 1, first - 1);
    }
    if (last + 1 < cols && cellAt(r, last + 1).ch == kWideTrailing) {
        cellAt(r, last + 1) = Cell{};  // its lead is about to be overwritten
        damage.mark(r, last + 1, last + 1);
    }
}

void Grid::advanceCursor(int width) {
    if (width == 2 && col + 1 < cols) {
        // The right half of a wide cluster. It owns nothing; it exists so the
        // column is occupied and so reflow can see the pair must move as one.
        cellAt(row, col + 1) = {kWideTrailing, pen};
        damage.mark(row, col + 1, col + 1);
        ++col;
    }
    if (col + 1 < cols) {
        ++col;
    } else {
        pendingWrap = true;  // DEC deferred wrap: stay on the last column
    }
}

void Grid::beginCluster(char32_t ch) {
    const int width = unicode::clusterWidth({&ch, 1}, ambiguous);
    if (width <= 0) {
        // A zero-width codepoint that UAX#29 says STARTS a cluster has nothing
        // to attach to — a combining mark opening a line is the usual case.
        // Dropped, as xterm drops it: a cell of its own would render an
        // isolated mark and would defeat reflow's unwritten-tail trim.
        return;
    }
    if (pendingWrap) {
        wrapToNextRow();
    }
    // A 2-column cluster is never split across the right edge: it wraps whole
    // and leaves the last column blank. A screen too narrow to hold one at all
    // keeps the lead and degrades to a single cell rather than looping.
    if (width == 2 && col + 1 >= cols && cols >= 2) {
        wrapToNextRow();
    }
    // BEFORE the write, and covering both cells a wide cluster will occupy:
    // afterwards the lead is already in place and the left-edge check would
    // read the cell we just wrote.
    breakWidePairs(row, col, width == 2 && col + 1 < cols ? col + 1 : col);
    cellAt(row, col) = {ch, pen};
    damage.mark(row, col, col);
    m_clusterRow = row;
    m_clusterCol = col;
    m_cluster[0] = ch;
    m_clusterLen = 1;
    m_clusterCh = ch;
    advanceCursor(width);
}

void Grid::appendToCluster(char32_t ch) {
    if (m_clusterLen >= kMaxClusterLen) {
        return;  // bounded; see kMaxClusterLen
    }
    // ED/EL rewrite cells without moving the cursor, so putChar's position
    // check cannot see them. If the anchored cell no longer holds what we put
    // there, it was erased or overwritten and this mark has nothing to join —
    // appending anyway would resurrect an erased cell with a stale glyph.
    Cell& lead = cellAt(m_clusterRow, m_clusterCol);
    if (lead.ch != m_clusterCh) {
        resetClusterAnchor();
        return;
    }
    m_cluster[m_clusterLen] = ch;
    ++m_clusterLen;

    m_clusterCh = m_clusters.intern({m_cluster.data(), m_clusterLen});
    lead.ch = m_clusterCh;
    damage.mark(m_clusterRow, m_clusterCol, m_clusterCol);

    // A continuation can WIDEN what it joins: VS16 promotes a text-presentation
    // base to a 2-cell emoji, and the second regional indicator turns a letter
    // into a flag. Claim the next column only while the cursor is still sitting
    // in it — if the lead went into the last column there is nowhere to grow
    // and the cluster stays one cell wide (recorded in docs/conformance.md).
    if (unicode::clusterWidth({m_cluster.data(), m_clusterLen}, ambiguous) == 2 &&
        row == m_clusterRow && col == m_clusterCol + 1 && !pendingWrap) {
        // Widening onto a cell that was the lead of another pair strands that
        // pair's trailing. The left edge is our OWN lead, so the span starts
        // there rather than at col.
        breakWidePairs(row, m_clusterCol, col);
        cellAt(row, col) = {kWideTrailing, pen};
        damage.mark(row, col, col);
        if (col + 1 < cols) {
            ++col;
        } else {
            pendingWrap = true;
        }
    }
}

void Grid::putChar(char32_t ch) {
    // The cluster stream is only contiguous if nothing moved the cursor since
    // the last write. Checking that here is what lets every other sequence
    // handler stay ignorant of clustering — see m_lastRow in the header.
    if (row != m_lastRow || col != m_lastCol || pendingWrap != m_lastWrap) {
        m_breaker.reset();
        resetClusterAnchor();
    }

    if (m_breaker.startsNewCluster(ch)) {
        beginCluster(ch);
    } else if (m_clusterCol >= 0) {
        appendToCluster(ch);
    }
    // else: a continuation with nothing to continue, because the cell it would
    // have joined was scrolled or erased out from under it. Dropped.

    m_lastRow = row;
    m_lastCol = col;
    m_lastWrap = pendingWrap;
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
    m_scrollback.push(std::move(line));
    // New history under a viewport that is scrolled up would shift everything
    // the user is reading by one row. Follow the content instead, until the
    // ring's cap stops giving ground.
    if (m_viewOffset > 0) {
        m_viewOffset = std::min(m_viewOffset + 1, maxViewOffset());
        // Every visible row is a different row now. DamageList addresses SCREEN
        // rows while the viewport is showing history, so partial damage would
        // describe the wrong lines entirely.
        damage.markAll();
    }
}

int Grid::maxViewOffset() const {
    // Bounded by the history that exists. lineCount() is LOGICAL lines and a
    // narrow screen wraps each into several, so this under-reports at small
    // widths — deliberately: over-reporting would scroll into rows that do not
    // exist and show blanks above the oldest output.
    return static_cast<int>(
        std::min<std::size_t>(m_scrollback.lineCount(), static_cast<std::size_t>(rows) * 1000));
}

bool Grid::scrollView(int delta) {
    const int wanted = std::clamp(m_viewOffset + delta, 0, maxViewOffset());
    if (wanted == m_viewOffset) {
        return false;  // no move, no repaint
    }
    m_viewOffset = wanted;
    damage.markAll();
    return true;
}

void Grid::scrollViewToBottom() {
    if (m_viewOffset != 0) {
        m_viewOffset = 0;
        damage.markAll();
    }
}

std::vector<Line> Grid::viewportRows() const {
    if (m_viewOffset <= 0) {
        return m_screen;
    }
    // The window is the `min(offset, rows)` history rows starting `offset` rows
    // up from the newest — NOT the last `rows` of history, which would render
    // the same screenful at every scroll depth.
    const auto offset = static_cast<std::size_t>(m_viewOffset);
    const auto want = std::min(offset, static_cast<std::size_t>(rows));
    std::vector<Line> out = m_scrollback.viewRows(cols, offset, want);
    // History may supply fewer rows than asked for; the live screen fills the
    // rest, so the viewport is always exactly `rows` tall.
    for (std::size_t i = 0; out.size() < static_cast<std::size_t>(rows) && i < m_screen.size();
         ++i) {
        out.push_back(m_screen[i]);
    }
    out.resize(static_cast<std::size_t>(rows), Line(cols));
    return out;
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
                std::move(m_screen[static_cast<std::size_t>(r) + 1]);
        }
        m_screen[static_cast<std::size_t>(scrollBottom)] = Line(cols);
    }
    // The row now at the top of the region is no longer a wrap continuation of
    // whatever scrolled away above it — UNLESS that line went to history, in
    // which case it did not scroll away at all and the continuity is real.
    // T21's ring needs exactly this flag to rejoin a wrapped line whose head
    // has already retired; reflow already treats screen row 0 as a line start
    // regardless, so keeping it costs the screen nothing.
    if (!toHistory) {
        m_screen[static_cast<std::size_t>(scrollTop)].wrappedFromPrev = false;
    }
    // Content moved under a cursor that did not move. A following combining
    // mark must not land on whatever slid into the anchored cell.
    resetClusterAnchor();
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
                std::move(m_screen[static_cast<std::size_t>(r) - 1]);
        }
        m_screen[static_cast<std::size_t>(scrollTop)] = Line(cols);
    }
    // A blank line was inserted at the top of the region, so the row below the
    // inserted block must not claim to continue it.
    if (scrollTop + n <= scrollBottom) {
        // Widen each operand BEFORE adding, not the sum: clang-tidy's
        // bugprone-misplaced-widening-cast gates the build, and it is right that
        // int arithmetic overflowing before a widening cast is a real bug class.
        m_screen[static_cast<std::size_t>(scrollTop) + static_cast<std::size_t>(n)]
            .wrappedFromPrev = false;
    }
    resetClusterAnchor();  // same reason as scrollRegionUp
    damage.markAll();
}

void Grid::resize(int newRows, int newCols) {
    // A minimized/0-height window must never produce a -1 cursor (UB).
    newRows = std::max(1, newRows);
    newCols = std::max(1, newCols);
    // Every cached position is about to describe a layout that no longer
    // exists, and the cluster under the cursor will not be in the same cell.
    m_breaker.reset();
    resetClusterAnchor();
    m_lastRow = -1;
    m_lastCol = -1;
    m_lastWrap = false;

    // COLUMNS FIRST: rewrapping changes how many rows the content needs, so
    // fitting rows before it would retire lines that reflow was about to merge
    // back onto the screen.
    //
    // useAlternateScreen() swaps the two vectors, so while a full-screen app is
    // up the NORMAL buffer is the one sitting in m_altScreen. Only that buffer
    // is rewrapped — the alternate screen belongs to an application that
    // redraws it on SIGWINCH, and rewrapping it would fight that redraw.
    if (cols != newCols) {
        std::vector<Line>& normal = m_onAlt ? m_altScreen : m_screen;
        const bool trackCursor = !m_onAlt;
        if (!normal.empty()) {
            ReflowResult rewrapped =
                reflow(normal, newCols, trackCursor ? row : -1, trackCursor ? col : 0);
            normal = std::move(rewrapped.lines);
            if (trackCursor) {
                row = rewrapped.cursorRow;
                col = rewrapped.cursorCol;
            }
        }
        // A narrowing rewrap splits lines and needs somewhere to put the extra
        // rows. Empty space at the BOTTOM absorbs it first: retiring content
        // off the top while the screen still has blank rows would scroll the
        // user's output into history for no reason. This is precisely what the
        // T8 prompt case was written to catch — without it, shrinking a window
        // pushes the prompt off screen instead of wrapping it.
        const auto keepAtLeast = static_cast<std::size_t>(trackCursor ? row + 1 : 1);
        while (normal.size() > static_cast<std::size_t>(newRows) && normal.size() > keepAtLeast &&
               isBlankLine(normal.back())) {
            normal.pop_back();
        }
        // The alternate buffer only gets truncated/padded.
        std::vector<Line>& alternate = m_onAlt ? m_screen : m_altScreen;
        for (Line& line : alternate) {
            line.cells.resize(static_cast<std::size_t>(newCols));
        }
        cols = newCols;
    }

    // Rows. xterm's Reallocate: "If the screen shrinks, remove lines off the
    // top of the buffer", under the default SouthWest gravity, for BOTH
    // buffers — getting this end wrong would destroy the shell prompt every
    // time a window shrank inside vim. Only the active buffer's retired rows
    // are history; the inactive one's are dropped, as before T20.
    // Rows retired below come from a DIFFERENT buffer than whatever the ring
    // last saw, so a wrappedFromPrev on the first of them must not glue itself
    // onto the previous buffer's last line.
    m_scrollback.breakLine();
    m_viewOffset = 0;  // the rewrapped layout invalidates every row offset
    while (m_screen.size() > static_cast<std::size_t>(newRows)) {
        pushToScrollback(std::move(m_screen.front()));
        m_screen.erase(m_screen.begin());
        row = std::max(0, row - 1);
    }
    while (m_screen.size() < static_cast<std::size_t>(newRows)) {
        m_screen.emplace_back(newCols);
    }
    // xterm reallocates the INACTIVE buffer too, so the normal screen survives
    // a resize taken while a full-screen app owns the alternate one.
    if (!m_altScreen.empty()) {
        m_scrollback.breakLine();  // second buffer, same reason
        while (m_altScreen.size() > static_cast<std::size_t>(newRows)) {
            // When the alternate screen is up, THIS vector holds the normal
            // buffer — and a narrowing rewrap can have just grown it past the
            // screen. Those are real scrollback lines: dropping them would eat
            // the top of the user's shell output the moment they resized a
            // window inside vim. Only genuine alternate-screen rows are lost.
            if (m_onAlt) {
                pushToScrollback(std::move(m_altScreen.front()));
            }
            m_altScreen.erase(m_altScreen.begin());
        }
        while (m_altScreen.size() < static_cast<std::size_t>(newRows)) {
            m_altScreen.emplace_back(newCols);
        }
    }
    rows = newRows;
    cols = newCols;
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
