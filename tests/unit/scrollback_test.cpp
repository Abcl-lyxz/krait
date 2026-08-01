#include "core/grid/cell.h"
#include "core/grid/grid.h"
#include "core/grid/reflow.h"
#include "core/grid/scrollback.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
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
    CHECK(sb.cellCount() <= std::size_t{64} * 10);  // widen BEFORE multiplying
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

TEST_CASE("scrollback: the visual row count matches what reflow produces", "[scrollback][shell]") {
    // The count is a second implementation of reflow()'s wrap rule (it walks
    // cells instead of copying them), and two implementations of one rule drift
    // unless something compares them. This is that something.
    //
    // It matters because jump-to-prompt turns a LOGICAL line index into a
    // viewport offset measured in VISUAL rows: under-count by one and the
    // prompt lands just above the top of the screen, i.e. invisible.
    for (const int cols : {1, 2, 3, 4, 5, 7, 8, 9, 80}) {
        CAPTURE(cols);
        Scrollback ring;
        std::vector<Line> logical;
        for (const std::u32string_view text : {
                 U"",
                 U"short",
                 U"exactly8",
                 U"a line that is quite a lot longer than eight columns",
             }) {
            Line line = makeLine(text, static_cast<int>(text.size()));
            logical.push_back(line);
            ring.push(std::move(line));
        }
        // A run of 2-column clusters, which is where a plain ceil(cells / cols)
        // goes wrong: a lead and its kWideTrailing half cannot straddle a row
        // boundary, so an odd width leaves one column of every row unused.
        Line wide(24);
        for (std::size_t i = 0; i < wide.cells.size(); i += 2) {
            wide.cells[i].ch = U'世';
            wide.cells[i + 1].ch = krait::core::vt::kWideTrailing;
        }
        logical.push_back(wide);
        ring.push(std::move(wide));

        // An INTERIOR hole, which CUP + EL punches. Both sides trim from the
        // END only, so these cells count — a counter that skipped every ch == 0
        // would agree on every case above and disagree here.
        Line hole(20);
        for (std::size_t i = 0; i < hole.cells.size(); ++i) {
            hole.cells[i].ch = (i >= 6 && i < 11) ? 0 : U'y';
        }
        logical.push_back(hole);
        ring.push(std::move(hole));

        // MIXED narrow and wide, so a pair does not land on the same parity in
        // every row. `$ ls 日本語` is this shape, not the homogeneous one above.
        Line mixed(0);
        mixed.cells.push_back(Cell{.ch = U'a'});
        mixed.cells.push_back(Cell{.ch = U'b'});
        for (int i = 0; i < 5; ++i) {
            mixed.cells.push_back(Cell{.ch = U'世'});
            mixed.cells.push_back(Cell{.ch = krait::core::vt::kWideTrailing});
            mixed.cells.push_back(Cell{.ch = U'c'});
        }
        logical.push_back(mixed);
        ring.push(std::move(mixed));

        // Malformed pairs: a lead whose trailing half ED blanked, two adjacent
        // trailing halves, and a lead at the very end with nothing after it.
        // The two implementations have to agree on garbage as well as on
        // well-formed content, because remote bytes produce both.
        Line orphan(6);
        orphan.cells[0].ch = U'z';
        orphan.cells[1].ch = U'世';
        orphan.cells[2].ch = krait::core::vt::kWideTrailing;
        orphan.cells[3].ch = krait::core::vt::kWideTrailing;
        orphan.cells[4].ch = U'世';
        logical.push_back(orphan);
        ring.push(std::move(orphan));

        // LAST, and deliberately so: an unwritten tail. Scrollback::push trims
        // the PREVIOUS line on every push, so only the newest line in the ring
        // is still untrimmed — which is the production shape, and which stops
        // being true the moment another line is pushed after it.
        Line tail(40);
        for (std::size_t i = 0; i < 12; ++i) {
            tail.cells[i].ch = U'x';
        }
        logical.push_back(tail);
        ring.push(std::move(tail));
        // reflow() over the same logical lines is the answer of record: it is
        // what viewRows() actually calls to serve the window.
        const std::size_t expected = krait::core::vt::reflow(logical, cols, -1, 0).lines.size();
        CHECK(ring.visualRowsFrom(0, cols) == expected);
    }
}

TEST_CASE("grid: jump-to-prompt reaches a mark that is deep in scrollback",
          "[scrollback][grid][shell]") {
    // A narrow grid so the prompt lines WRAP: counting logical lines instead of
    // visual rows passes on a wide grid and fails here, which is exactly the
    // shape of bug this pins.
    Grid g(4, 8);
    const auto type = [&g](std::u32string_view text) {
        for (const char32_t ch : text) {
            g.putChar(ch);
        }
        g.linefeed();
        g.cursorSet(g.row, 0);
    };

    g.markPrompt(krait::core::vt::kMarkPromptStart);
    type(U"$ first command with a long line");
    for (int i = 0; i < 20; ++i) {
        type(U"output line");
    }
    g.markPrompt(krait::core::vt::kMarkPromptStart);
    type(U"$ second");
    for (int i = 0; i < 20; ++i) {
        type(U"more output");
    }
    REQUIRE(g.scrollbackSize() > 20);

    // From the live screen, the previous prompt is the second one.
    const std::optional<std::size_t> second = g.prevPrompt(g.viewTopLine());
    REQUIRE(second.has_value());
    g.scrollToLine(*second);
    CHECK(g.viewOffset() > 0);
    // The line asked for is the one at the TOP of what is drawn — the whole
    // point of jumping to a prompt is seeing its output underneath it.
    REQUIRE_FALSE(g.viewportRows().empty());
    CHECK((g.viewportRows()[0].marks & krait::core::vt::kMarkPromptStart) != 0);
    // And the anchor moved with it, so pressing the binding again walks BACK
    // rather than returning to the same prompt.
    CHECK(g.viewTopLine() == *second);

    const std::optional<std::size_t> first = g.prevPrompt(g.viewTopLine());
    REQUIRE(first.has_value());
    CHECK(*first < *second);
    g.scrollToLine(*first);
    CHECK((g.viewportRows()[0].marks & krait::core::vt::kMarkPromptStart) != 0);

    // Output arriving must not drag the view off the prompt it was parked on.
    // Asserted as "the prompt is STILL the top row", not as "the offset did not
    // shrink": the failure mode is the offset failing to grow with the content,
    // which a `>=` check passes straight through.
    const int parked = g.viewOffset();
    type(U"late output");
    CHECK(g.viewOffset() > parked);
    CHECK(g.viewTopLine() == *first);
    CHECK((g.viewportRows()[0].marks & krait::core::vt::kMarkPromptStart) != 0);

    // A wheel notch down and back up returns to where it was. A clamp that read
    // its own ceiling off the current offset would eat rows on the way down and
    // never give them back.
    const int here = g.viewOffset();
    g.scrollView(-3);
    g.scrollView(3);
    CHECK(g.viewOffset() == here);

    // Forward again, and past the last prompt there is simply nothing.
    CHECK(g.nextPrompt(g.viewTopLine()).has_value());
    g.scrollViewToBottom();
    CHECK_FALSE(g.nextPrompt(g.viewTopLine()).has_value());
}

TEST_CASE("grid: a prompt mark dirties the row it lands on", "[scrollback][grid][shell]") {
    // A mark changes no cell, so nothing on the write path would ever dirty the
    // row — and a renderer that draws prompts differently would show the change
    // whenever something unrelated next touched that line, or never.
    Grid g(4, 8);
    g.damage.clear();
    REQUIRE_FALSE(g.damage.all());
    REQUIRE(g.damage.spans()[0].col0 == -1);

    g.markPrompt(krait::core::vt::kMarkPromptStart);
    CHECK(g.damage.spans()[0].col0 == 0);

    // Re-marking with bits already set changes nothing, so it must not cost a
    // repaint: a shell that emits A twice is common and repaint is not free.
    g.damage.clear();
    g.markPrompt(krait::core::vt::kMarkPromptStart);
    CHECK(g.damage.spans()[0].col0 == -1);

    // An exit status is drawn too, and lands on the prompt's row.
    g.putChar(U'x');
    g.linefeed();
    g.damage.clear();
    g.setCommandExit(3);
    CHECK(g.absoluteLineAt(0).exitCode == 3);
    CHECK(g.damage.spans()[0].col0 == 0);
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
