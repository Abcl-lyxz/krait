#include "core/grid/reflow.h"

#include "core/grid/cell.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace krait::core::vt {

namespace {

// Joins one logical line's rows end to end and reports where the cursor landed
// inside the join. Rows are read through their own cells.size() rather than an
// assumed width, so a buffer mid-resize with ragged rows cannot walk off one.
struct Joined {
    std::vector<Cell> cells;
    long long cursorOffset = -1;  // -1 when the cursor is not on this line
};

Joined joinLogicalLine(std::span<const Line> rows, std::size_t first, std::size_t last,
                       int cursorRow, int cursorCol) {
    Joined joined;
    for (std::size_t r = first; r < last; ++r) {
        if (cursorRow >= 0 && static_cast<std::size_t>(cursorRow) == r) {
            joined.cursorOffset = static_cast<long long>(joined.cells.size()) + cursorCol;
        }
        const std::vector<Cell>& src = rows[r].cells;
        joined.cells.insert(joined.cells.end(), src.begin(), src.end());
    }
    return joined;
}

}  // namespace

ReflowResult reflow(std::span<const Line> rows, int newCols, int cursorRow, int cursorCol) {
    const int cols = std::max(1, newCols);
    const auto ucols = static_cast<std::size_t>(cols);

    ReflowResult out;
    bool cursorPlaced = false;

    std::size_t first = 0;
    while (first < rows.size()) {
        // A logical line runs until the next row that is not a continuation.
        // Row 0 always starts one even if it claims wrappedFromPrev: its
        // predecessor already scrolled into history and is not ours to rejoin.
        std::size_t last = first + 1;
        while (last < rows.size() && rows[last].wrappedFromPrev) {
            ++last;
        }

        Joined joined = joinLogicalLine(rows, first, last, cursorRow, cursorCol);

        // Shell-integration marks (line.h) belong to the LOGICAL line, so they
        // are gathered here and re-attached to whichever row heads the re-split
        // run below. That is the whole reason a mark lives on the Line: a
        // resize rebuilds every visual row, and anything keyed by row number
        // would now be pointing at different text. OR rather than taking
        // rows[first] alone so a mark that landed on a continuation row — a
        // long prompt wrapping, with C arriving on the second row — is carried
        // rather than dropped.
        std::uint8_t marks = 0;
        int exitCode = -1;
        for (std::size_t r = first; r < last; ++r) {
            marks |= rows[r].marks;
            if (exitCode < 0) {
                exitCode = rows[r].exitCode;
            }
        }
        bool headEmitted = false;

        // Trim the unwritten tail — rule 1 in the header. Walks in from the end
        // only, so interior holes survive.
        std::size_t contentEnd = joined.cells.size();
        while (contentEnd > 0 && joined.cells[contentEnd - 1].ch == 0) {
            --contentEnd;
        }
        joined.cells.resize(contentEnd);

        std::vector<Cell> pending;
        pending.reserve(ucols);
        // Screen row 0 may legitimately continue a line whose head already
        // scrolled into history. We cannot rejoin it, but clearing its flag
        // would destroy the wrap point T8 recorded for T21 to join against.
        bool continuation = first == 0 && rows[0].wrappedFromPrev;

        auto flushRow = [&]() {
            Line line(cols);
            std::copy(pending.begin(), pending.end(), line.cells.begin());
            line.wrappedFromPrev = continuation;
            if (!headEmitted) {
                line.marks = marks;
                line.exitCode = exitCode;
                headEmitted = true;
            }
            out.lines.push_back(std::move(line));
            pending.clear();
        };

        for (std::size_t k = 0; k < joined.cells.size();) {
            // Rule 2: a lead cell is wide exactly when a kWideTrailing cell
            // follows it. No width table is consulted here on purpose.
            const bool wide =
                (k + 1 < joined.cells.size()) && isWideTrailing(joined.cells[k + 1].ch);
            // A pair cannot be represented on a one-column screen at all; keep
            // the lead and drop the trailing half rather than emit an orphan
            // kWideTrailing that nothing owns.
            const bool keepPair = wide && ucols >= 2;
            const std::size_t need = keepPair ? 2 : 1;

            if (pending.size() + need > ucols) {
                flushRow();
                continuation = true;
            }
            // Match the whole cluster, not just its lead: k steps by 2 over a
            // wide pair, so a cursor parked on the TRAILING half would never
            // equal k and would fall through to the past-content branch below.
            const auto step = static_cast<long long>(wide ? 2 : 1);
            if (joined.cursorOffset >= 0 && !cursorPlaced &&
                joined.cursorOffset >= static_cast<long long>(k) &&
                joined.cursorOffset < static_cast<long long>(k) + step) {
                out.cursorRow = static_cast<int>(out.lines.size());
                out.cursorCol =
                    static_cast<int>(pending.size()) +
                    (keepPair ? static_cast<int>(joined.cursorOffset - static_cast<long long>(k))
                              : 0);
                cursorPlaced = true;
            }
            pending.push_back(joined.cells[k]);
            if (keepPair) {
                pending.push_back(joined.cells[k + 1]);
            }
            k += wide ? 2 : 1;
        }

        // The cursor can sit past the content it followed — an active prompt is
        // exactly that. Place it arithmetically in the blank tail we just
        // trimmed away, spilling onto further rows if the new width is narrow.
        if (joined.cursorOffset >= 0 && !cursorPlaced) {
            long long pos = static_cast<long long>(pending.size()) +
                            (joined.cursorOffset - static_cast<long long>(contentEnd));
            // Clamp into the row this logical line actually ends on. Spilling
            // onto rows we never emit would report the cursor inside the NEXT
            // logical line, and would inflate the caller's "keep at least this
            // many rows" floor enough to disable blank-row absorption.
            pos = std::clamp(pos, 0LL, static_cast<long long>(cols) - 1);
            out.cursorRow = static_cast<int>(out.lines.size());
            out.cursorCol = static_cast<int>(pos);
            cursorPlaced = true;
        }

        // Always emit at least one row: an empty logical line still occupies a
        // visual row, and collapsing it would silently eat blank lines.
        flushRow();
        first = last;
    }

    return out;
}

}  // namespace krait::core::vt
