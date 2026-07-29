#include "core/grid/grid.h"
#include "core/parser/sgr.h"
#include <catch2/catch_test_macros.hpp>

using krait::core::vt::Grid;

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

TEST_CASE("grid: scrollback caps at kMaxScrollback", "[grid]") {
    Grid g(2, 2);
    for (std::size_t i = 0; i < Grid::kMaxScrollback + 50; ++i) {
        g.linefeed();
    }
    CHECK(g.scrollbackSize() == Grid::kMaxScrollback);
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

// --- Reflow scaffold (M1). These document the DESIRED behavior; naive
// resize does not rewrap yet, so they are [!mayfail] until reflow lands. ---

TEST_CASE("grid: resize wider rejoins a wrapped logical line", "[grid][!mayfail]") {
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

TEST_CASE("grid: resize narrower must not tear a wide char at the boundary", "[grid][!mayfail]") {
    // Wide-char cells arrive with the width tables (utf8proc work); until
    // then this scaffold fails by construction to keep the case visible.
    FAIL("wide-char-aware reflow not implemented (M1; needs width tables)");
}

TEST_CASE("grid: shrinking columns keeps the active prompt readable", "[grid][!mayfail]") {
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
