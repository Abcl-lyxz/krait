#include "core/grid/scrollback.h"

#include "core/grid/cell.h"
#include "core/grid/reflow.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <utility>

namespace krait::core::vt {

namespace {

// Drops the never-written tail. Only ch == 0 goes: a printed space is content
// (see reflow.h). Retiring a 240-column row for two characters of output would
// otherwise store 238 blanks per line, which is most of the cell budget.
void trimTail(Line& line) {
    std::size_t end = line.cells.size();
    while (end > 0 && line.cells[end - 1].ch == 0) {
        --end;
    }
    line.cells.resize(end);
    // Same reason as the clamp in push(): a 240-column row retired for two
    // characters counts as 2 cells but would keep holding 240.
    line.cells.shrink_to_fit();
}

}  // namespace

void Scrollback::push(Line&& row) {
    const bool continues = row.wrappedFromPrev && !m_forceBreak && !m_lines.empty();
    m_forceBreak = false;
    if (continues) {
        // A continuation: glue it onto the logical line already in the ring.
        Line& back = m_lines.back();
        m_cells -= back.cells.size();
        back.cells.insert(back.cells.end(), row.cells.begin(), row.cells.end());
        // One line may not eat the whole budget on its own — a stream that
        // never emits a newline is otherwise unbounded.
        if (back.cells.size() > m_maxCells) {
            back.cells.resize(m_maxCells);
            // resize() down never releases capacity, so without this the line
            // holds ~1.5x the budget it is counted as — the cap would bound
            // the accounting rather than the memory it exists to bound.
            back.cells.shrink_to_fit();
        }
        m_cells += back.cells.size();
    } else {
        // A new logical line starts, so the previous one is finished and its
        // blank tail can go. Trimming here rather than on read keeps the cell
        // count honest, which is what the cap is enforced against.
        if (!m_lines.empty()) {
            m_cells -= m_lines.back().cells.size();
            trimTail(m_lines.back());
            m_cells += m_lines.back().cells.size();
        }
        m_lines.push_back(std::move(row));
        // Stored lines are logical, so the flag has no meaning in the ring.
        m_lines.back().wrappedFromPrev = false;
        m_cells += m_lines.back().cells.size();
    }
    evict();
}

void Scrollback::evict() {
    // Oldest first. The size() > 1 guard keeps one line alive even when it
    // alone exceeds the cell budget — push() already clamped it, and evicting
    // down to nothing would throw away the newest line to satisfy a bound the
    // next push would immediately break again.
    while ((m_lines.size() > m_maxLines || m_cells > m_maxCells) && m_lines.size() > 1) {
        m_cells -= m_lines.front().cells.size();
        m_lines.pop_front();
    }
}

std::vector<Line> Scrollback::viewRows(int cols, std::size_t fromEnd, std::size_t count) const {
    if (count == 0 || m_lines.empty() || cols < 1) {
        return {};
    }
    const std::size_t needRows = fromEnd + count;
    // Every logical line yields at least one visual row, so `needRows` of them
    // always cover the window. That bounds how far back we walk.
    //
    // The cell budget bounds the other axis, and it is the one that matters for
    // hostile input: a single logical line is capped only by the CELL budget,
    // and a stream that never emits a newline builds exactly that line — the
    // coalescing in push() is designed to. Without this, one scroll step would
    // rewrap millions of cells to show a screenful, every frame.
    const std::size_t cellBudget = (needRows + 1) * static_cast<std::size_t>(cols);

    std::vector<Line> logical;  // newest-first while building
    std::size_t cells = 0;
    for (std::size_t i = m_lines.size(); i-- > 0 && logical.size() < needRows;) {
        Line copy = m_lines[i];
        if (copy.cells.size() > cellBudget) {
            // Keep the TAIL. The window sits at the end of history, and where
            // an unbroken multi-megabyte line happens to break a screenful
            // earlier is not observable.
            copy.cells.erase(copy.cells.begin(),
                             copy.cells.end() - static_cast<std::ptrdiff_t>(cellBudget));
        }
        cells += copy.cells.size();
        logical.push_back(std::move(copy));
        if (cells >= cellBudget) {
            break;
        }
    }
    std::reverse(logical.begin(), logical.end());

    ReflowResult wrapped = reflow(logical, cols, -1, 0);
    const std::size_t have = wrapped.lines.size();
    const std::size_t back = std::min(fromEnd, have);
    const std::size_t begin = have - back;
    const std::size_t end = std::min(have, begin + count);
    return {std::make_move_iterator(wrapped.lines.begin() + static_cast<std::ptrdiff_t>(begin)),
            std::make_move_iterator(wrapped.lines.begin() + static_cast<std::ptrdiff_t>(end))};
}

void Scrollback::setCaps(std::size_t maxLines, std::size_t maxCells) {
    m_maxLines = std::max<std::size_t>(1, maxLines);
    m_maxCells = std::max<std::size_t>(1, maxCells);
    evict();
}

void Scrollback::clear() {
    m_lines.clear();
    m_cells = 0;
}

}  // namespace krait::core::vt
