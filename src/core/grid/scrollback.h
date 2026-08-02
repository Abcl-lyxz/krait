#pragma once

#include "core/grid/line.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

    // OSC 133 ; D writes the exit status onto the line the command was typed
    // on, and by the time a command finishes that line has usually scrolled off
    // the screen (line.h). A setter rather than a mutable `Line&` because this
    // class accounts for every cell it holds (m_cells) — handing out a
    // reference would make that invariant enforceable only by comment.
    void setExitCode(std::size_t i, int code) { m_lines[i].exitCode = code; }

    // A WINDOW of visual rows, wrapped at `cols`: `count` rows ending
    // `fromEnd` rows before the newest. fromEnd == count == rows gives the
    // screenful immediately above the live screen.
    //
    // A window rather than a tail because a viewport scrolled N rows up needs
    // rows [V-N, V-N+count), and "the last count rows" answers a different
    // question — it renders the identical screenful at every depth.
    std::vector<Line> viewRows(int cols, std::size_t fromEnd, std::size_t count) const;

    // How many VISUAL rows logical lines [first, end) occupy at `cols` — the
    // same count viewRows() would produce for them, which is the whole point:
    // a viewport offset is measured in visual rows, so this is what turns "the
    // prompt is logical line 412" into "scroll back N rows" (T67's
    // jump-to-prompt). Counting logical lines instead would land short by one
    // row for every line that wrapped, i.e. exactly on long command lines.
    std::size_t visualRowsFrom(std::size_t first, int cols) const;

    // One line's share of that. Separate so a caller walking history backwards
    // accumulates instead of re-summing the tail per step, which is the
    // difference between O(cells) and O(cells squared).
    std::size_t visualRowsOfLine(std::size_t i, int cols) const;

    // The whole ring's visual height — visualRowsFrom(0, cols), CACHED.
    //
    // The cache is the point. Grid::maxViewOffset() calls this on every wheel
    // tick AND on every line of output that arrives under a scrolled-back
    // viewport, so an uncached O(cells) walk there would be a full scan of
    // history on the output hot path. Keyed on the width and on a counter that
    // every mutating method below bumps, which is sound because there is no
    // other way to change what this counts: lineAt() hands out a const&, and
    // setExitCode cannot change a row count.
    std::size_t visualRowCount(int cols) const;

    // How many logical lines this ring has ever STARTED. Monotone: eviction
    // and clear() do not decrease it, they advance m_dropped instead.
    //
    // It exists because `lineAt`'s index is not an identity. Every eviction
    // shifts every index down by one, so an index recorded now names different
    // text later — the landmine CLAUDE.md records for scrollback, and the
    // reason OSC 133 marks live on the Line and not in a side table keyed by
    // row. A value captured from here stays comparable to one read later
    // forever, which is what lets Grid remember WHERE an open prompt is
    // without remembering a position that eviction would invalidate.
    std::uint64_t linesEverStarted() const { return m_dropped + m_lines.size(); }

    // Turns a captured linesEverStarted() back into a current index: that
    // line's position if it is still here, `lineCount()` if it has not been
    // pushed yet, 0 if it has been evicted. A FLOOR for a backwards walk —
    // Grid::setCommandExit stops there rather than scanning all of history
    // when the mark it went looking for was cleared out from under it.
    std::size_t indexOfStable(std::uint64_t stable) const {
        if (stable <= m_dropped) {
            return 0;
        }
        return std::min<std::size_t>(static_cast<std::size_t>(stable - m_dropped), m_lines.size());
    }

    // Forces the next push() to start a new logical line even if the row
    // carries wrappedFromPrev. resize() retires rows from BOTH buffers back to
    // back, and a continuation flag from one buffer must never glue itself to
    // the other buffer's last line.
    void breakLine() { m_forceBreak = true; }

    void setCaps(std::size_t maxLines, std::size_t maxCells);
    void clear();

  private:
    void evict();

    std::deque<Line> m_lines;
    std::size_t m_cells = 0;
    std::size_t m_maxLines = kDefaultMaxLines;
    std::size_t m_maxCells = kDefaultMaxCells;
    bool m_forceBreak = false;

    // Logical lines that have left the front of the ring, ever. The offset
    // between the stable numbering above and the current deque index.
    std::uint64_t m_dropped = 0;

    // Bumped by every method that can change a row count. Starts at 1 so the
    // zero-initialised cache below is a guaranteed miss on the first call.
    std::uint64_t m_generation = 1;
    mutable std::uint64_t m_rowCacheGeneration = 0;
    mutable int m_rowCacheCols = 0;
    mutable std::size_t m_rowCacheRows = 0;
};

}  // namespace krait::core::vt
