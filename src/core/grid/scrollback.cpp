#include "core/grid/scrollback.h"

#include "core/grid/cell.h"
#include "core/grid/reflow.h"

#include <algorithm>
#include <cstddef>
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
}

}  // namespace

void Scrollback::push(Line&& row) {
    if (row.wrappedFromPrev && !m_lines.empty()) {
        // A continuation: glue it onto the logical line already in the ring.
        Line& back = m_lines.back();
        m_cells -= back.cells.size();
        back.cells.insert(back.cells.end(), row.cells.begin(), row.cells.end());
        // One line may not eat the whole budget on its own — a stream that
        // never emits a newline is otherwise unbounded.
        if (back.cells.size() > m_maxCells) {
            back.cells.resize(m_maxCells);
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

std::vector<Line> Scrollback::tailRows(int cols, std::size_t count) const {
    if (count == 0 || m_lines.empty() || cols < 1) {
        return {};
    }
    // A logical line can only produce MORE visual rows than itself, never
    // fewer, so the last `count` logical lines always contain the last `count`
    // visual rows. That bounds the rewrap to what is being asked for instead
    // of the whole ring.
    //
    // ponytail: O(count) per call with no cache. A viewport asks for a
    // screenful, so that is a screenful of rewrap per scroll step. Cache the
    // wrapped rows against (cols, revision) if a bench ever says it matters.
    const std::size_t take = std::min(count, m_lines.size());
    const std::vector<Line> logical(m_lines.end() - static_cast<std::ptrdiff_t>(take),
                                    m_lines.end());

    ReflowResult wrapped = reflow(logical, cols, -1, 0);
    if (wrapped.lines.size() > count) {
        wrapped.lines.erase(wrapped.lines.begin(),
                            wrapped.lines.end() - static_cast<std::ptrdiff_t>(count));
    }
    return std::move(wrapped.lines);
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
