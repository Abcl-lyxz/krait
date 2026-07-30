#pragma once

#include "core/grid/cell.h"
#include "core/grid/damage.h"
#include "core/grid/line.h"

#include <array>
#include <cstddef>
#include <deque>
#include <vector>

namespace krait::core::vt {

// One DECSC / mode-1049 saved-cursor slot. xterm's SavedCursor (cursor.c
// CursorSave2) keeps position, SGR attributes, origin mode, the pending-wrap
// flag and charset state — but NOT the scrolling margins, which are screen
// state rather than cursor state (DEC agrees; see vt100.net DECSC).
struct SavedCursor {
    int row = 0;  // ABSOLUTE, even when origin mode was on at save time
    int col = 0;
    Attr pen;
    bool originMode = false;
    bool pendingWrap = false;
    bool g1Invoked = false;
};

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

    // Scrolling region (DECSTBM), 0-based and INCLUSIVE. Defaults to the whole
    // screen. DEC VT510: "You cannot perform scrolling outside the margins."
    int scrollTop = 0;
    int scrollBottom;         // rows - 1 at construction
    bool originMode = false;  // DECOM: addressing is relative to scrollTop

    bool inScrollRegion(int r) const { return r >= scrollTop && r <= scrollBottom; }

    // Whether lines scrolled off the top belong in scrollback. xterm's gate is
    // the TOP margin alone — `scroll_all_lines = (scrollWidget && !whichBuf &&
    // screen->top_marg == 0)` — NOT a full-screen region. An app reserving a
    // footer (top=0, bottom<rows-1) still wants its history kept; one that
    // pins a header (top>0) is managing its own viewport and must not pollute
    // history. The alternate screen never captures at all — full-screen apps
    // own their viewport and their redraws are not history — EXCEPT that a
    // row-shrinking resize() retires active-buffer rows unconditionally, which
    // xterm also does (ScreenResize gates that copy on gravity, not whichBuf).
    bool capturesScrollback() const { return scrollTop == 0 && !m_onAlt; }

    // Scroll the region by n lines. Lines pushed out of the region are lost
    // (DEC: "Lines scrolled off the page are lost"), except off the top of a
    // full-screen region, which goes to scrollback. Blank lines carry no
    // attributes. n is clamped to the region height.
    void scrollRegionUp(int n);
    void scrollRegionDown(int n);

    // The ONE cursor-addressing path: CUP, HVP, VPA, DECSTBM's home and
    // DECRC all route here so origin mode is applied in exactly one place.
    // `r`/`c` are 0-based. With origin mode on, `r` is relative to scrollTop
    // and the result cannot leave the region; with it off, `r` addresses the
    // whole page (xterm cursor.c CursorSet).
    void cursorSet(int r, int c);

    // Alternate screen (mode 1049). Only the CELLS swap: in xterm the margins,
    // pen and cursor are single TScreen members and only `whichBuf` selects
    // between two cell arrays — so a region set on the normal screen is still
    // in effect on the alternate one.
    void useAlternateScreen(bool on);

    bool onAlternateScreen() const { return m_onAlt; }

    // Blanks every row of the ACTIVE buffer (mode 1049's "clearing it first").
    //
    // This and useAlternateScreen() allocate, and every CSI handler that calls
    // them is `noexcept` — a settled decision, not an oversight: a bad_alloc
    // while growing one screen's worth of cells means the process is already
    // finished, and there is no per-sequence recovery worth writing. The whole
    // handler family (handleErase, handleScroll, handleMode) is noexcept on the
    // same grounds. Revisit only if the grid ever becomes unbounded.
    void eraseScreen();

    // Two slots, indexed by which buffer is active. 1049 saves BEFORE it
    // switches, so its save lands in the normal screen's slot and clobbers a
    // pending ESC 7 — exactly what xterm's sc[whichBuf] does.
    void saveCursor();
    void restoreCursor();

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
    // Retires a line off the top of the screen into scrollback.
    void pushToScrollback(Line&& line);

    std::vector<Line> m_screen;
    std::deque<Line> m_scrollback;
    // The inactive buffer. Empty until mode 1049 is first set; from then on it
    // holds whichever buffer is NOT on screen (m_screen and this are swapped).
    std::vector<Line> m_altScreen;
    std::array<SavedCursor, 2> m_saved{};  // [0] normal, [1] alternate
    bool m_onAlt = false;
};

}  // namespace krait::core::vt
