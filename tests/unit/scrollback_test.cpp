#include "core/grid/cell.h"
#include "core/grid/grid.h"
#include "core/grid/scrollback.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using krait::core::vt::Cell;
using krait::core::vt::Color;
using krait::core::vt::Grid;
using krait::core::vt::Line;
using krait::core::vt::Scrollback;

namespace {

Line makeLine(std::u32string_view text, int cols, bool wrapped = false) {
    Line line(cols);
    for (std::size_t i = 0; i < text.size() && i < static_cast<std::size_t>(cols); ++i) {
        line.cells[i].ch = text[i] == U'.' ? 0 : text[i];
    }
    line.wrappedFromPrev = wrapped;
    return line;
}

std::string lineAscii(const Line& line) {
    std::string out;
    for (const Cell& cell : line.cells) {
        out.push_back(cell.ch == 0 ? '.' : static_cast<char>(cell.ch));
    }
    return out;
}

}  // namespace

TEST_CASE("scrollback: continuation rows coalesce into one logical line", "[scrollback]") {
    Scrollback sb;
    sb.push(makeLine(U"abcde", 5));
    sb.push(makeLine(U"fgh..", 5, true));

    // One LOGICAL line, not two visual rows — that is what survives a resize.
    REQUIRE(sb.lineCount() == 1);
    CHECK(lineAscii(sb.lineAt(0)) == "abcdefgh..");
}

TEST_CASE("scrollback: a finished line drops its unwritten tail", "[scrollback]") {
    Scrollback sb;
    sb.push(makeLine(U"hi........", 10));
    sb.push(makeLine(U"next......", 10));  // starting a new line finishes the first

    CHECK(lineAscii(sb.lineAt(0)) == "hi");
    CHECK(sb.cellCount() < 20);
}

TEST_CASE("scrollback: printed spaces are not trimmed", "[scrollback]") {
    Scrollback sb;
    sb.push(makeLine(U"hi  ......", 10));
    sb.push(makeLine(U"x.........", 10));

    CHECK(lineAscii(sb.lineAt(0)) == "hi  ");
}

TEST_CASE("scrollback: rewraps history at the width asked for", "[scrollback]") {
    Scrollback sb;
    sb.push(makeLine(U"abcde", 5));
    sb.push(makeLine(U"fgh..", 5, true));

    // Stored logical, re-split on read — the same history, two widths.
    const std::vector<Line> wide = sb.viewRows(10, 4, 4);
    REQUIRE(wide.size() == 1);
    CHECK(lineAscii(wide[0]) == "abcdefgh..");

    const std::vector<Line> narrow = sb.viewRows(4, 4, 4);
    REQUIRE(narrow.size() == 2);
    CHECK(lineAscii(narrow[0]) == "abcd");
    CHECK(lineAscii(narrow[1]) == "efgh");
    CHECK(narrow[1].wrappedFromPrev);
}

TEST_CASE("scrollback: the window moves with the scroll depth", "[scrollback]") {
    // Found by review: "the last N rows" answers a different question and
    // renders the identical screenful however far up you scroll.
    Scrollback sb;
    for (char32_t ch : {U'a', U'b', U'c', U'd', U'e'}) {
        Line line(4);
        line.cells[0].ch = ch;
        sb.push(std::move(line));
    }

    const std::vector<Line> newest = sb.viewRows(4, 2, 2);
    REQUIRE(newest.size() == 2);
    CHECK(newest[0].cells[0].ch == U'd');

    const std::vector<Line> deeper = sb.viewRows(4, 4, 2);
    REQUIRE(deeper.size() == 2);
    CHECK(deeper[0].cells[0].ch == U'b');
    CHECK(deeper[1].cells[0].ch == U'c');
}

TEST_CASE("scrollback: a window read does not rewrap an endless line", "[scrollback]") {
    // A stream that never emits a newline builds ONE logical line at the full
    // cell budget. Reading a screenful out of it must not touch all of it.
    Scrollback sb;
    sb.setCaps(1'000, 200'000);
    sb.push(makeLine(U"start.....", 10));
    for (int i = 0; i < 30'000; ++i) {
        sb.push(makeLine(U"0123456789", 10, /*wrapped=*/true));
    }
    REQUIRE(sb.cellCount() > 100'000);

    const std::vector<Line> window = sb.viewRows(80, 24, 24);
    CHECK(window.size() == 24);
    for (const Line& line : window) {
        CHECK(line.cells.size() == 80);
    }
}

TEST_CASE("scrollback: breakLine stops a continuation gluing across buffers", "[scrollback]") {
    Scrollback sb;
    sb.push(makeLine(U"alt.", 4));
    sb.breakLine();
    sb.push(makeLine(U"norm", 4, /*wrapped=*/true));

    // Without breakLine the alt-screen row and the shell row become one line.
    CHECK(sb.lineCount() == 2);
}

TEST_CASE("scrollback: a flood keeps O(cap) memory", "[scrollback]") {
    // The acceptance criterion for T21. 200x the line cap of real content.
    Scrollback sb;
    sb.setCaps(64, 100'000);
    for (int i = 0; i < 12'800; ++i) {
        sb.push(makeLine(U"0123456789", 10));
    }

    CHECK(sb.lineCount() == 64);
    CHECK(sb.cellCount() <= 64 * 10);
}

TEST_CASE("scrollback: one endless line cannot outgrow the cell budget", "[scrollback]") {
    // The bound a line cap alone does not give: a stream that never emits a
    // newline adds no LINES at all, so only the cell budget stops it.
    Scrollback sb;
    sb.setCaps(1'000, 500);
    sb.push(makeLine(U"start.....", 10));
    for (int i = 0; i < 2'000; ++i) {
        sb.push(makeLine(U"0123456789", 10, /*wrapped=*/true));
    }

    CHECK(sb.lineCount() == 1);
    CHECK(sb.cellCount() <= 500);
}

TEST_CASE("scrollback: shrinking the cap evicts immediately", "[scrollback]") {
    Scrollback sb;
    for (int i = 0; i < 100; ++i) {
        sb.push(makeLine(U"line......", 10));
    }
    REQUIRE(sb.lineCount() == 100);

    sb.setCaps(10, Scrollback::kDefaultMaxCells);
    CHECK(sb.lineCount() == 10);
}

TEST_CASE("grid: retired rows land in history as logical lines", "[scrollback][grid]") {
    Grid g(2, 4);
    for (char32_t ch : {U'a', U'b', U'c', U'd', U'e', U'f'}) {
        g.putChar(ch);  // wraps at 4 columns
    }
    g.linefeed();
    g.linefeed();

    REQUIRE(g.scrollbackSize() >= 1);
    // "abcdef" wrapped across two rows on screen; history keeps it as one line.
    CHECK(lineAscii(g.scrollbackAt(0)).substr(0, 6) == "abcdef");
}

TEST_CASE("grid: the viewport clamps and only repaints when it moves", "[scrollback][grid]") {
    Grid g(3, 8);
    for (int i = 0; i < 10; ++i) {
        g.putChar(U'x');
        g.linefeed();
    }
    REQUIRE(g.scrollbackSize() > 0);

    CHECK(g.viewOffset() == 0);
    CHECK(g.scrollView(2));
    CHECK(g.viewOffset() == 2);

    // Already at the bottom: a wheel tick past it must not cost a repaint.
    g.scrollViewToBottom();
    CHECK_FALSE(g.scrollView(-5));
    CHECK(g.viewOffset() == 0);
}

TEST_CASE("grid: the viewport is always exactly `rows` tall", "[scrollback][grid]") {
    Grid g(4, 8);
    for (int i = 0; i < 20; ++i) {
        g.putChar(static_cast<char32_t>(U'a' + (i % 26)));
        g.linefeed();
    }

    CHECK(g.viewportRows().size() == 4);
    g.scrollView(2);
    CHECK(g.viewportRows().size() == 4);
    g.scrollView(1000);  // clamped
    CHECK(g.viewportRows().size() == 4);
}

TEST_CASE("grid: scrolling deeper shows different rows", "[scrollback][grid]") {
    Grid g(4, 8);
    for (int i = 0; i < 20; ++i) {
        g.cursorSet(g.row, 0);  // putChar advances the column; keep the mark at 0
        g.putChar(static_cast<char32_t>(U'a' + i));
        g.linefeed();
    }
    REQUIRE(g.scrollbackSize() > 8);

    g.scrollView(4);
    const std::vector<Line> shallow = g.viewportRows();
    g.scrollView(6);  // now 10 up
    const std::vector<Line> deep = g.viewportRows();

    CHECK(shallow[0].cells[0].ch != deep[0].cells[0].ch);
}

TEST_CASE("cell: the packed Color round-trips every kind", "[scrollback][cell]") {
    CHECK(Color{}.kind() == Color::Kind::Default);

    const Color idx = Color::indexed(200);
    CHECK(idx.kind() == Color::Kind::Indexed);
    CHECK(idx.index() == 200);

    const Color rgb = Color::rgb(0xAB'CD'EF);
    CHECK(rgb.kind() == Color::Kind::Rgb);
    CHECK(rgb.rgb() == 0xAB'CD'EFU);

    // Distinct kinds with the same payload must not compare equal.
    CHECK_FALSE(Color::indexed(1) == Color::rgb(1));
    CHECK(Color::indexed(7) == Color::indexed(7));
}
