#pragma once

#include "core/grid/cell.h"
#include "core/grid/damage.h"
#include "core/grid/line.h"

#include <cstddef>
#include <deque>
#include <vector>

namespace krait::core::vt {

// Screen + scrollback (T8). Replaces the T6/T7 StubGrid: same open members
// the CSI handlers mutate, but with real wrap, scroll, damage, and resize.
// DECAWM (autowrap) is always on for now, with DEC deferred-wrap semantics:
// writing the last column leaves the cursor there and sets pendingWrap; the
// NEXT printable wraps. Cursor movement clears pendingWrap.
class Grid {
  public:
    static constexpr std::size_t kMaxScrollback = 10'000;  // lines; ring cap

    Grid(int rowCount, int colCount);

    int rows;  // visual screen size
    int cols;
    int row = 0;  // cursor, 0-based
    int col = 0;
    bool pendingWrap = false;
    bool g1Invoked = false;  // SO/SI shift state; charsets are later work
    int bells = 0;
    Attr pen;
    DamageList damage;

    Cell& cellAt(int r, int c);
    const Cell& cellAt(int r, int c) const;
    Line& lineAt(int r);
    const Line& lineAt(int r) const;

    // Writes at the cursor with the current pen, honoring deferred wrap.
    void putChar(char32_t ch);
    // Cursor down one row; scrolls (into scrollback) at the bottom.
    void linefeed();

    std::size_t scrollbackSize() const { return m_scrollback.size(); }

    const Line& scrollbackAt(std::size_t i) const { return m_scrollback[i]; }

    // Naive until M1 reflow: rows shrink pushes top lines into scrollback,
    // growth appends blank rows; column changes truncate/pad each row in
    // place (wrap flags kept, content NOT rewrapped). Cursor clamped.
    void resize(int newRows, int newCols);

  private:
    void scrollUp();

    std::vector<Line> m_screen;
    std::deque<Line> m_scrollback;
};

}  // namespace krait::core::vt
