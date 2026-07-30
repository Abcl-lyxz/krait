#include "core/grid/grid.h"
#include "core/parser/sgr.h"
#include <catch2/catch_test_macros.hpp>

using krait::core::vt::Grid;
using krait::core::vt::isWideTrailing;
using krait::core::vt::Scrollback;

TEST_CASE("grid: deferred wrap at the right margin", "[grid]") {
    Grid g(4, 10);
    for (int i = 0; i < 10; ++i) {
        g.putChar(U'a' + static_cast<char32_t>(i));
    }
    // DEC deferred wrap: cursor parks on the last column, no wrap yet.
    CHECK(g.row == 0);
    CHECK(g.col == 9);
    CHECK(g.pendingWrap);

    g.putChar(U'K');  // the next printable wraps
    CHECK(g.row == 1);
    CHECK(g.col == 1);
    CHECK(g.cellAt(1, 0).ch == U'K');
    CHECK(g.lineAt(1).wrappedFromPrev);
    CHECK_FALSE(g.lineAt(0).wrappedFromPrev);
}

TEST_CASE("grid: cursor motion cancels pending wrap", "[grid]") {
    Grid g(4, 4);
    for (int i = 0; i < 4; ++i) {
        g.putChar(U'x');
    }
    REQUIRE(g.pendingWrap);
    g.col = 0;  // CR-style motion via handlers sets this and clears the flag
    g.pendingWrap = false;
    g.putChar(U'y');
    CHECK(g.row == 0);  // overwrote in place, no wrap happened
    CHECK(g.cellAt(0, 0).ch == U'y');
}

TEST_CASE("grid: linefeed at the bottom scrolls into scrollback", "[grid]") {
    Grid g(3, 5);
    g.putChar(U'A');
    for (int i = 0; i < 3; ++i) {
        g.linefeed();
    }
    CHECK(g.row == 2);  // clamped at the bottom, screen scrolled once
    REQUIRE(g.scrollbackSize() == 1);
    CHECK(g.scrollbackAt(0).cells[0].ch == U'A');
    CHECK(g.cellAt(0, 0).ch == 0);  // the 'A' row left the screen
    CHECK(g.damage.all());
}

TEST_CASE("grid: scroll region confines scrolling to the margins", "[grid][scroll]") {
    Grid g(5, 4);
    for (int r = 0; r < 5; ++r) {  // label every row with its index
        g.row = r;
        g.col = 0;
        g.putChar(U'0' + static_cast<char32_t>(r));
    }
    g.scrollTop = 1;
    g.scrollBottom = 3;
    g.scrollRegionUp(1);
    // Rows outside the margins must not move at all.
    CHECK(g.cellAt(0, 0).ch == U'0');
    CHECK(g.cellAt(4, 0).ch == U'4');
    CHECK(g.cellAt(1, 0).ch == U'2');
    CHECK(g.cellAt(2, 0).ch == U'3');
    CHECK(g.cellAt(3, 0).ch == 0);  // blanked bottom of the region
    // A region-limited scroll is an app managing its own viewport; those
    // lines are lost, never pushed into history.
    CHECK(g.scrollbackSize() == 0);
}

TEST_CASE("grid: scroll region down loses lines off the bottom", "[grid][scroll]") {
    Grid g(4, 4);
    for (int r = 0; r < 4; ++r) {
        g.row = r;
        g.col = 0;
        g.putChar(U'a' + static_cast<char32_t>(r));
    }
    g.scrollRegionDown(1);
    CHECK(g.cellAt(0, 0).ch == 0);  // blank inserted at the top
    CHECK(g.cellAt(1, 0).ch == U'a');
    CHECK(g.cellAt(3, 0).ch == U'c');  // the 'd' row fell off and is gone
    CHECK(g.scrollbackSize() == 0);    // never below the screen
}

TEST_CASE("grid: scrollback capture is gated on the TOP margin only", "[grid][scroll]") {
    // xterm's gate is top_marg == 0, not a full-screen region: an app that
    // reserves a footer still wants its history kept.
    Grid g(4, 4);
    g.putChar(U'A');
    g.scrollBottom = 2;  // footer reserved on the last row, top margin still 0
    REQUIRE(g.capturesScrollback());
    g.scrollRegionUp(1);
    REQUIRE(g.scrollbackSize() == 1);
    CHECK(g.scrollbackAt(0).cells[0].ch == U'A');

    // Pinning a header (top > 0) suppresses capture.
    Grid h(4, 4);
    h.scrollTop = 1;
    h.scrollBottom = 3;
    CHECK_FALSE(h.capturesScrollback());
    h.scrollRegionUp(1);
    CHECK(h.scrollbackSize() == 0);
}

TEST_CASE("grid: scroll amount clamps to the region height", "[grid][scroll]") {
    Grid g(6, 4);
    g.scrollTop = 2;
    g.scrollBottom = 3;
    for (int r = 0; r < 6; ++r) {
        g.row = r;
        g.col = 0;
        g.putChar(U'0' + static_cast<char32_t>(r));
    }
    g.scrollRegionUp(99);              // far more than the 2-row region
    CHECK(g.cellAt(1, 0).ch == U'1');  // untouched above
    CHECK(g.cellAt(2, 0).ch == 0);     // region fully blanked
    CHECK(g.cellAt(3, 0).ch == 0);
    CHECK(g.cellAt(4, 0).ch == U'4');  // untouched below
    g.scrollRegionUp(0);               // no-op, must not blank anything
    CHECK(g.cellAt(4, 0).ch == U'4');
}

TEST_CASE("grid: linefeed below the region does not scroll it", "[grid][scroll]") {
    Grid g(4, 4);
    g.scrollTop = 0;
    g.scrollBottom = 1;  // region is the top two rows only
    g.row = 3;           // cursor parked below the region, on the last row
    g.linefeed();
    // Outside the margins there is nothing to scroll and nowhere to go.
    CHECK(g.row == 3);
    CHECK(g.scrollbackSize() == 0);
}

TEST_CASE("grid: resize re-clamps stale margins", "[grid][scroll]") {
    Grid g(10, 4);
    g.scrollTop = 4;
    g.scrollBottom = 9;
    g.resize(3, 4);  // margins now describe rows that no longer exist
    CHECK(g.scrollTop == 0);
    CHECK(g.scrollBottom == 2);
    g.scrollRegionUp(1);  // must not index past the screen
    SUCCEED();
}

TEST_CASE("grid: resize clears origin mode with the margins", "[grid][scroll]") {
    // xterm's ScreenResize does resetMargins() then UIntClr(flags, ORIGIN) —
    // origin mode outliving its margins would address a region that is gone.
    Grid g(10, 4);
    g.scrollTop = 4;
    g.scrollBottom = 9;
    g.originMode = true;
    g.resize(8, 4);
    CHECK_FALSE(g.originMode);
    // Re-narrow before probing: resize() zeroes scrollTop too, so without a
    // fresh margin a surviving originMode would still land on row 0 and this
    // would assert nothing.
    g.scrollTop = 4;
    g.scrollBottom = 7;
    g.cursorSet(0, 0);  // absolute home, not the new top margin
    CHECK(g.row == 0);
}

TEST_CASE("grid: alternate screen never captures scrollback", "[grid][alt]") {
    // xterm util.c gates scrollback on `!screen->whichBuf` as well as on the
    // top margin: a full-screen app's redraws are not history.
    Grid g(2, 4);
    g.useAlternateScreen(true);
    REQUIRE(g.capturesScrollback() == false);
    g.putChar(U'A');
    g.linefeed();
    g.linefeed();  // at the bottom, so this scrolls the region
    CHECK(g.scrollbackSize() == 0);
    // Back on the normal screen the same scroll DOES capture.
    g.useAlternateScreen(false);
    g.row = 0;
    g.col = 0;
    g.putChar(U'B');
    g.row = g.rows - 1;
    g.linefeed();
    REQUIRE(g.scrollbackSize() == 1);
    CHECK(g.scrollbackAt(0).cells[0].ch == U'B');
}

TEST_CASE("grid: switching buffers preserves the other one", "[grid][alt]") {
    Grid g(2, 4);
    g.putChar(U'N');
    g.useAlternateScreen(true);
    CHECK(g.cellAt(0, 0).ch == 0);  // the alternate buffer starts blank
    g.row = 0;
    g.col = 0;
    g.putChar(U'A');
    g.useAlternateScreen(false);
    CHECK(g.cellAt(0, 0).ch == U'N');  // normal content survived
    g.useAlternateScreen(true);
    CHECK(g.cellAt(0, 0).ch == U'A');  // and so did the alternate's
}

TEST_CASE("grid: resize reshapes the inactive buffer too", "[grid][alt]") {
    // xterm's ScreenResize reallocates editBuf_index[!whichBuf], so a resize
    // taken while an app owns the alternate screen must not leave the normal
    // buffer the wrong shape — cellAt() would index out of bounds on return.
    Grid g(4, 6);
    // On the LAST row, so top-trimming keeps it. Asserting content matters:
    // useAlternateScreen() re-allocates a blank buffer of the right shape when
    // m_altScreen is empty, so a shape-only check passes even with the whole
    // reshape path deleted.
    g.row = 3;
    g.col = 0;
    g.putChar(U'N');
    g.useAlternateScreen(true);
    g.resize(2, 3);
    g.useAlternateScreen(false);
    REQUIRE(g.rows == 2);
    for (int r = 0; r < g.rows; ++r) {
        CHECK(g.lineAt(r).cells.size() == 3);
    }
    CHECK(g.cellAt(1, 0).ch == U'N');
}

TEST_CASE("grid: shrinking keeps the BOTTOM of the inactive buffer", "[grid][alt]") {
    // xterm Reallocate: "If the screen shrinks, remove lines off the top of the
    // buffer" — SouthWest gravity, and it runs for the inactive buffer too.
    // Trimming the bottom instead would destroy the shell prompt every time a
    // window shrank inside vim.
    Grid g(4, 4);
    for (int r = 0; r < 4; ++r) {
        g.row = r;
        g.col = 0;
        g.putChar(U'0' + static_cast<char32_t>(r));
    }
    g.useAlternateScreen(true);
    g.resize(2, 4);
    g.useAlternateScreen(false);
    CHECK(g.cellAt(0, 0).ch == U'2');
    CHECK(g.cellAt(1, 0).ch == U'3');
}

TEST_CASE("grid: saved cursor round-trips through origin mode", "[grid][alt]") {
    // The slot stores an ABSOLUTE row; restoreCursor() must take the top margin
    // back out before cursorSet() re-applies it, or the row drifts by scrollTop.
    Grid g(10, 8);
    g.scrollTop = 3;
    g.scrollBottom = 8;
    g.originMode = true;
    g.cursorSet(1, 2);  // region row 2 -> absolute row 4
    REQUIRE(g.row == 4);
    g.saveCursor();
    g.cursorSet(0, 0);
    REQUIRE(g.row == 3);
    g.restoreCursor();
    CHECK(g.row == 4);
    CHECK(g.col == 2);
}

TEST_CASE("grid: the two saved-cursor slots are independent", "[grid][alt]") {
    // xterm indexes sc[] by whichBuf, so ESC 7 on the alternate screen cannot
    // clobber what mode 1049 saved on the way in.
    Grid g(10, 8);
    g.cursorSet(5, 5);
    g.saveCursor();  // slot 0
    g.useAlternateScreen(true);
    g.cursorSet(1, 1);
    g.saveCursor();  // slot 1
    g.cursorSet(9, 7);
    g.restoreCursor();
    CHECK(g.row == 1);
    g.useAlternateScreen(false);
    g.restoreCursor();
    CHECK(g.row == 5);
    CHECK(g.col == 5);
}

TEST_CASE("grid: cursorSet clamps into the region only in origin mode", "[grid][scroll]") {
    Grid g(10, 8);
    g.scrollTop = 3;
    g.scrollBottom = 6;
    g.cursorSet(9, 0);  // DECOM off: the whole page is addressable
    CHECK(g.row == 9);
    g.originMode = true;
    g.cursorSet(99, 0);  // DECOM on: clamped to the bottom margin
    CHECK(g.row == 6);
}

TEST_CASE("grid: damage coalesces per-row spans and clears", "[grid]") {
    Grid g(3, 10);
    g.putChar(U'A');
    g.putChar(U'B');
    g.col = 7;
    g.putChar(U'C');
    REQUIRE_FALSE(g.damage.all());
    CHECK(g.damage.spans()[0].col0 == 0);
    CHECK(g.damage.spans()[0].col1 == 7);
    CHECK(g.damage.spans()[1].col0 == -1);  // untouched row is clean

    g.damage.clear();
    CHECK(g.damage.spans()[0].col0 == -1);
    CHECK_FALSE(g.damage.all());
}

TEST_CASE("grid: logical line spans wrapped rows, hard break ends it", "[grid]") {
    Grid g(4, 4);
    for (int i = 0; i < 6; ++i) {
        g.putChar(U'0' + static_cast<char32_t>(i));
    }
    CHECK(g.lineAt(1).wrappedFromPrev);  // rows 0..1 form one logical line
    g.linefeed();
    g.col = 0;
    g.putChar(U'X');
    CHECK_FALSE(g.lineAt(2).wrappedFromPrev);  // LF made a hard break
}

TEST_CASE("grid: full-row erase clears the wrap flag", "[grid]") {
    Grid g(4, 4);
    for (int i = 0; i < 6; ++i) {
        g.putChar(U'x');
    }
    REQUIRE(g.lineAt(1).wrappedFromPrev);
    g.row = 1;
    krait::core::vt::Params p;
    p.values[0] = 2;
    p.count = 1;
    REQUIRE(krait::core::vt::handleErase(g, p, {}, 'K'));  // EL 2
    CHECK_FALSE(g.lineAt(1).wrappedFromPrev);
}

TEST_CASE("grid: resize clamps to at least 1x1", "[grid]") {
    Grid g(4, 10);
    g.resize(0, 0);
    CHECK(g.rows == 1);
    CHECK(g.cols == 1);
    CHECK(g.row == 0);
    CHECK(g.col == 0);
    g.putChar(U'a');  // must not crash / write out of bounds
    CHECK(g.cellAt(0, 0).ch == U'a');
}

TEST_CASE("grid: scrollback caps at the ring's line limit", "[grid]") {
    // T21 moved the cap onto Scrollback, which counts LOGICAL lines and also
    // bounds total cells — see scrollback_test.cpp for the second bound.
    Grid g(2, 2);
    for (std::size_t i = 0; i < Scrollback::kDefaultMaxLines + 50; ++i) {
        g.linefeed();
    }
    CHECK(g.scrollbackSize() == Scrollback::kDefaultMaxLines);
}

TEST_CASE("grid: naive resize keeps content near the cursor", "[grid]") {
    Grid g(4, 10);
    g.putChar(U'h');
    g.putChar(U'i');
    g.row = 3;
    g.col = 9;
    g.resize(2, 5);
    CHECK(g.rows == 2);
    CHECK(g.cols == 5);
    CHECK(g.row <= g.rows - 1);
    CHECK(g.col <= g.cols - 1);
    // Shrinking rows pushed the top (incl. "hi") into scrollback.
    REQUIRE(g.scrollbackSize() == 2);
    CHECK(g.scrollbackAt(0).cells[0].ch == U'h');
    CHECK(g.damage.all());
}

// --- Reflow (T20). These three were the [!mayfail] scaffold T8 left behind to
// keep the cases visible; T20 made them required. Do not re-tag them. ---

TEST_CASE("grid: resize wider rejoins a wrapped logical line", "[grid][reflow]") {
    Grid g(4, 5);
    for (int i = 0; i < 7; ++i) {
        g.putChar(U'a' + static_cast<char32_t>(i));
    }
    REQUIRE(g.lineAt(1).wrappedFromPrev);
    g.resize(4, 10);
    // Desired: the 7-char logical line fits one row again.
    CHECK(g.cellAt(0, 5).ch == U'f');
    CHECK_FALSE(g.lineAt(1).wrappedFromPrev);
}

TEST_CASE("grid: resize narrower must not tear a wide char at the boundary", "[grid][reflow]") {
    // U+4E16, East Asian Wide. Spelled numerically so the test never depends
    // on the source file's encoding surviving a toolchain change.
    constexpr char32_t kCjk = 0x4E16;

    Grid g(4, 6);
    for (char32_t ch : {U'a', U'b', U'c', U'd'}) {
        g.putChar(ch);
    }
    g.putChar(kCjk);
    // Six columns exactly: four letters, then the pair in columns 4 and 5.
    REQUIRE(g.cellAt(0, 4).ch == kCjk);
    REQUIRE(isWideTrailing(g.cellAt(0, 5).ch));

    g.resize(4, 5);

    // The pair no longer fits after "abcd", and splitting it would put half a
    // glyph on each row. It moves whole and leaves column 4 blank instead.
    CHECK(g.cellAt(0, 3).ch == U'd');
    CHECK(g.cellAt(0, 4).ch == 0);
    CHECK(g.lineAt(1).wrappedFromPrev);
    CHECK(g.cellAt(1, 0).ch == kCjk);
    CHECK(isWideTrailing(g.cellAt(1, 1).ch));

    // The invariant that matters more than any single position: a trailing
    // half never exists without its lead immediately to the left.
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 5; ++c) {
            if (isWideTrailing(g.cellAt(r, c).ch)) {
                REQUIRE(c > 0);
                CHECK_FALSE(g.cellAt(r, c - 1).ch == 0);
                CHECK_FALSE(isWideTrailing(g.cellAt(r, c - 1).ch));
            }
        }
    }
}

TEST_CASE("grid: shrinking columns keeps the active prompt readable", "[grid][reflow]") {
    Grid g(4, 10);
    const char32_t prompt[] = {U'>', U' ', U'l', U's'};
    for (char32_t ch : prompt) {
        g.putChar(ch);
    }
    g.resize(4, 3);
    // Desired: reflow wraps the prompt line instead of truncating it.
    CHECK(g.lineAt(1).wrappedFromPrev);
    CHECK(g.cellAt(1, 0).ch == U's');
}

TEST_CASE("grid: overwriting a wide cluster's lead does not strand its trailing", "[grid][wide]") {
    // Found by the fuzzer, minimised to: U+FFFD, a wide char, CR, a wide char.
    // The second write put its own trailing on the first pair's LEAD, leaving
    // that pair's trailing beside it — two adjacent trailing halves. That is
    // not cosmetic: the renderer walks LEFT from a trailing to find the lead
    // whose glyph it belongs to, and finding another trailing is how a wide
    // cell bug becomes an out-of-bounds read.
    Grid g(2, 8);
    constexpr char32_t kWide = 0x4E16;  // East Asian Wide

    g.putChar(U'�');   // one cell, column 0
    g.putChar(kWide);  // lead at 1, trailing at 2
    REQUIRE(isWideTrailing(g.cellAt(0, 2).ch));

    g.cursorSet(0, 0);
    g.putChar(kWide);  // lead at 0, trailing at 1 — over the old lead

    CHECK(g.cellAt(0, 0).ch == kWide);
    CHECK(isWideTrailing(g.cellAt(0, 1).ch));
    // The orphan is blanked, not left behind.
    CHECK_FALSE(isWideTrailing(g.cellAt(0, 2).ch));
    CHECK(g.cellAt(0, 2).ch == 0);
}

TEST_CASE("grid: overwriting a wide cluster's trailing does not strand its lead", "[grid][wide]") {
    // The mirror case. A lead with no trailing draws a two-column glyph over a
    // cell that now belongs to something else.
    Grid g(2, 8);
    constexpr char32_t kWide = 0x4E16;

    g.putChar(kWide);  // lead at 0, trailing at 1
    REQUIRE(isWideTrailing(g.cellAt(0, 1).ch));

    g.cursorSet(0, 1);
    g.putChar(U'x');  // lands on the trailing half

    CHECK(g.cellAt(0, 1).ch == U'x');
    CHECK(g.cellAt(0, 0).ch == 0);  // the orphaned lead is blanked
}

TEST_CASE("grid: a wide cluster overwriting another wide cluster leaves no orphan",
          "[grid][wide]") {
    // Offset by one, so the new pair straddles both halves of the old one.
    Grid g(2, 8);
    constexpr char32_t kWide = 0x4E16;

    g.putChar(U'a');
    g.putChar(kWide);  // lead at 1, trailing at 2
    g.cursorSet(0, 2);
    g.putChar(kWide);  // lead at 2, trailing at 3 — over the old trailing

    CHECK(g.cellAt(0, 0).ch == U'a');
    CHECK(g.cellAt(0, 1).ch == 0);  // the old lead, orphaned and blanked
    CHECK(g.cellAt(0, 2).ch == kWide);
    CHECK(isWideTrailing(g.cellAt(0, 3).ch));
    // The invariant the fuzz harness asserts: no trailing follows a trailing.
    for (int c = 1; c < 8; ++c) {
        INFO("column " << c);
        const bool adjacentTrailings =
            isWideTrailing(g.cellAt(0, c).ch) && isWideTrailing(g.cellAt(0, c - 1).ch);
        CHECK_FALSE(adjacentTrailings);
    }
}
