#include "core/grid/cluster_pool.h"
#include "core/grid/grid.h"
#include "core/grid/reflow.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using krait::core::vt::Cell;
using krait::core::vt::ClusterPool;
using krait::core::vt::Grid;
using krait::core::vt::isWideTrailing;
using krait::core::vt::kWideTrailing;
using krait::core::vt::Line;
using krait::core::vt::reflow;
using krait::core::vt::ReflowResult;

namespace {

// '.' is an UNWRITTEN cell (ch == 0), which is the distinction the whole trim
// rule turns on — a printed space is ' ' and must survive.
Line makeLine(std::u32string_view text, int cols, bool wrapped = false) {
    Line line(cols);
    for (std::size_t i = 0; i < text.size() && i < static_cast<std::size_t>(cols); ++i) {
        line.cells[i].ch = text[i] == U'.' ? 0 : text[i];
    }
    line.wrappedFromPrev = wrapped;
    return line;
}

std::string rowAscii(const Line& line) {
    std::string out;
    for (const Cell& cell : line.cells) {
        if (cell.ch == 0) {
            out.push_back('.');
        } else if (isWideTrailing(cell.ch)) {
            out.push_back('>');
        } else if (cell.ch < 128) {
            out.push_back(static_cast<char>(cell.ch));
        } else {
            out.push_back('#');
        }
    }
    return out;
}

}  // namespace

TEST_CASE("reflow: rejoins a wrapped logical line and re-splits at the new width", "[reflow]") {
    std::vector<Line> rows{makeLine(U"abcde", 5), makeLine(U"fgh..", 5, true)};

    const ReflowResult wider = reflow(rows, 10, -1, 0);
    REQUIRE(wider.lines.size() == 1);
    CHECK(rowAscii(wider.lines[0]) == "abcdefgh..");
    CHECK_FALSE(wider.lines[0].wrappedFromPrev);

    const ReflowResult narrower = reflow(rows, 4, -1, 0);
    REQUIRE(narrower.lines.size() == 2);
    CHECK(rowAscii(narrower.lines[0]) == "abcd");
    CHECK(rowAscii(narrower.lines[1]) == "efgh");
    CHECK_FALSE(narrower.lines[0].wrappedFromPrev);
    CHECK(narrower.lines[1].wrappedFromPrev);
}

TEST_CASE("reflow: trims the unwritten tail but keeps printed spaces", "[reflow]") {
    // "hi" then two REAL spaces, then unwritten cells. A naive trim that
    // stopped at the last non-space would eat output the program produced.
    std::vector<Line> rows{makeLine(U"hi  .....", 9)};

    const ReflowResult out = reflow(rows, 9, -1, 0);
    REQUIRE(out.lines.size() == 1);
    CHECK(rowAscii(out.lines[0]) == "hi  .....");

    const ReflowResult narrow = reflow(rows, 3, -1, 0);
    REQUIRE(narrow.lines.size() == 2);
    CHECK(rowAscii(narrow.lines[0]) == "hi ");
    CHECK(rowAscii(narrow.lines[1]) == " ..");
}

TEST_CASE("reflow: preserves an interior hole an application punched", "[reflow]") {
    // CUP + EL leaves a gap in the middle. The trim walks in from the END
    // only, so the gap has to survive as blank cells rather than collapse.
    std::vector<Line> rows{makeLine(U"ab..ef...", 9)};

    const ReflowResult out = reflow(rows, 6, -1, 0);
    REQUIRE(out.lines.size() == 1);
    CHECK(rowAscii(out.lines[0]) == "ab..ef");
}

TEST_CASE("reflow: an empty logical line still occupies a row", "[reflow]") {
    std::vector<Line> rows{makeLine(U"aa...", 5), makeLine(U".....", 5), makeLine(U"bb...", 5)};

    const ReflowResult out = reflow(rows, 5, -1, 0);
    REQUIRE(out.lines.size() == 3);
    CHECK(rowAscii(out.lines[1]) == ".....");
}

TEST_CASE("reflow: carries the cursor to the same character", "[reflow]") {
    std::vector<Line> rows{makeLine(U"abcde", 5), makeLine(U"fgh..", 5, true)};

    // Cursor on 'g' — row 1, column 1, i.e. logical offset 6.
    const ReflowResult wider = reflow(rows, 10, 1, 1);
    CHECK(wider.cursorRow == 0);
    CHECK(wider.cursorCol == 6);

    const ReflowResult narrower = reflow(rows, 4, 1, 1);
    CHECK(narrower.cursorRow == 1);
    CHECK(narrower.cursorCol == 2);
}

TEST_CASE("reflow: keeps a cursor parked past the end of its content", "[reflow]") {
    // The active-prompt shape: content is 4 cells, the cursor sits at offset 4
    // in the blank tail that the trim is about to delete.
    std::vector<Line> rows{makeLine(U"> ls......", 10)};

    const ReflowResult out = reflow(rows, 3, 0, 4);
    CHECK(out.cursorRow == 1);
    CHECK(out.cursorCol == 1);
    REQUIRE(out.lines.size() == 2);
    CHECK(rowAscii(out.lines[0]) == "> l");
    CHECK(rowAscii(out.lines[1]) == "s..");
}

TEST_CASE("reflow: never emits a trailing half without its lead", "[reflow]") {
    Line row(4);
    row.cells[0].ch = U'a';
    row.cells[1].ch = 0x4E16;  // East Asian Wide
    row.cells[2].ch = kWideTrailing;
    row.cells[3].ch = U'b';
    std::vector<Line> rows{std::move(row)};

    // Three columns hold 'a' plus the pair exactly, so nothing has to move.
    const ReflowResult exact = reflow(rows, 3, -1, 0);
    REQUIRE(exact.lines.size() == 2);
    CHECK(rowAscii(exact.lines[0]) == "a#>");
    CHECK(rowAscii(exact.lines[1]) == "b..");

    // Two columns cannot hold 'a' and the pair. Splitting would put half a
    // glyph on each row, so the pair moves down whole and column 1 stays blank.
    const ReflowResult out = reflow(rows, 2, -1, 0);
    REQUIRE(out.lines.size() == 3);
    CHECK(rowAscii(out.lines[0]) == "a.");
    CHECK(rowAscii(out.lines[1]) == "#>");
    CHECK(rowAscii(out.lines[2]) == "b.");
    CHECK(out.lines[1].wrappedFromPrev);
    CHECK(out.lines[2].wrappedFromPrev);

    // One column cannot represent a pair at all. The lead survives alone
    // rather than an orphan trailing cell being emitted.
    const ReflowResult single = reflow(rows, 1, -1, 0);
    for (const Line& line : single.lines) {
        CHECK_FALSE(isWideTrailing(line.cells[0].ch));
    }
}

TEST_CASE("cluster pool: single codepoints never take a slot", "[reflow][cluster]") {
    ClusterPool pool;
    const std::vector<char32_t> single{U'a'};
    CHECK(pool.intern(single) == U'a');
    CHECK(pool.size() == 0);
    CHECK(pool.lookup(U'a').empty());
}

TEST_CASE("cluster pool: identical clusters intern once", "[reflow][cluster]") {
    ClusterPool pool;
    const std::vector<char32_t> cluster{U'e', 0x0301};

    const char32_t first = pool.intern(cluster);
    const char32_t second = pool.intern(cluster);
    CHECK(first == second);
    CHECK(pool.size() == 1);

    const std::span<const char32_t> back = pool.lookup(first);
    REQUIRE(back.size() == 2);
    CHECK(back[0] == U'e');
    CHECK(back[1] == 0x0301);
}

TEST_CASE("cluster pool: degrades to the base codepoint when full", "[reflow][cluster]") {
    // The bound that makes hostile input safe: a stream of distinct clusters
    // must stop allocating, not stop the process.
    ClusterPool pool;
    for (std::uint32_t i = 0; i < ClusterPool::kMaxClusters; ++i) {
        const std::vector<char32_t> cluster{U'A', static_cast<char32_t>(0x10000 + i)};
        REQUIRE(pool.intern(cluster) != U'A');
    }
    REQUIRE(pool.size() == ClusterPool::kMaxClusters);

    const std::vector<char32_t> overflow{U'A', 0x30000};
    CHECK(pool.intern(overflow) == U'A');
    CHECK(pool.size() == ClusterPool::kMaxClusters);
}

TEST_CASE("grid: a combining mark joins the previous cell instead of taking one",
          "[reflow][cluster]") {
    Grid g(2, 8);
    g.putChar(U'e');
    g.putChar(0x0301);  // COMBINING ACUTE ACCENT

    CHECK(g.col == 1);  // one cell consumed, not two
    const std::span<const char32_t> cluster = g.clusters().lookup(g.cellAt(0, 0).ch);
    REQUIRE(cluster.size() == 2);
    CHECK(cluster[0] == U'e');
    CHECK(cluster[1] == 0x0301);
    CHECK(g.cellAt(0, 1).ch == 0);
}

TEST_CASE("grid: a mark that widens its cluster claims the next column", "[reflow][cluster]") {
    // U+2764 is one cell as text; VS16 promotes it to an emoji, which is two.
    Grid g(2, 8);
    g.putChar(0x2764);
    REQUIRE(g.col == 1);
    g.putChar(0xFE0F);  // VARIATION SELECTOR-16

    CHECK(g.col == 2);
    CHECK(isWideTrailing(g.cellAt(0, 1).ch));
}

TEST_CASE("grid: a regional-indicator pair is one two-column flag", "[reflow][cluster]") {
    Grid g(2, 8);
    g.putChar(0x1F1F9);  // REGIONAL INDICATOR SYMBOL LETTER T
    g.putChar(0x1F1ED);  // REGIONAL INDICATOR SYMBOL LETTER H

    CHECK(g.col == 2);
    CHECK(isWideTrailing(g.cellAt(0, 1).ch));
    const std::span<const char32_t> cluster = g.clusters().lookup(g.cellAt(0, 0).ch);
    REQUIRE(cluster.size() == 2);
    CHECK(cluster[0] == 0x1F1F9);
}

TEST_CASE("grid: a cursor jump breaks the cluster stream", "[reflow][cluster]") {
    // The self-healing check in putChar: nothing told the grid that CUP moved
    // the cursor, it noticed. Without this the mark would join a cell the
    // application had already walked away from.
    Grid g(2, 8);
    g.putChar(U'e');
    g.cursorSet(1, 0);
    g.putChar(0x0301);

    CHECK(g.cellAt(0, 0).ch == U'e');  // untouched
    CHECK(g.cellAt(1, 0).ch == 0);     // the orphan mark was dropped, not drawn
}

TEST_CASE("grid: a wide cluster wraps whole rather than straddling the edge", "[reflow][cluster]") {
    Grid g(3, 4);
    for (char32_t ch : {U'a', U'b', U'c'}) {
        g.putChar(ch);
    }
    g.putChar(0x4E16);  // no room for two columns at column 3

    CHECK(g.cellAt(0, 3).ch == 0);  // left blank, not half a glyph
    CHECK(g.lineAt(1).wrappedFromPrev);
    CHECK(g.cellAt(1, 0).ch == 0x4E16);
    CHECK(isWideTrailing(g.cellAt(1, 1).ch));
}

TEST_CASE("grid: an erased cell does not come back as a cluster", "[reflow][cluster]") {
    // ED/EL rewrite cells WITHOUT moving the cursor, so putChar's position
    // check cannot see them. Found by review, not by the cursor-jump test
    // above: appending anyway resurrected the erased glyph.
    Grid g(2, 8);
    g.putChar(U'a');
    g.cellAt(0, 0) = {};  // what ED/EL do to the cell
    g.putChar(0x0301);

    CHECK(g.cellAt(0, 0).ch == 0);
}

TEST_CASE("reflow: places a cursor parked on a wide trailing half", "[reflow]") {
    // k steps by 2 over a pair, so an offset landing on the trailing half
    // never equals k and used to fall through to the past-content branch.
    Line row(8);
    for (std::size_t i = 0; i < 8; i += 2) {
        row.cells[i].ch = 0x4E00;
        row.cells[i + 1].ch = kWideTrailing;
    }
    std::vector<Line> rows{std::move(row)};

    const ReflowResult out = reflow(rows, 4, 0, 3);  // cursor on a trailing half
    CHECK(out.cursorRow == 0);
    CHECK(out.cursorCol == 3);
}

TEST_CASE("reflow: keeps row 0's wrap flag when its head is already history", "[reflow]") {
    std::vector<Line> rows{makeLine(U"tail.", 5, true)};

    const ReflowResult out = reflow(rows, 5, -1, 0);
    REQUIRE(out.lines.size() == 1);
    CHECK(out.lines[0].wrappedFromPrev);
}

TEST_CASE("grid: a stored cluster survives a resize", "[reflow][cluster]") {
    Grid g(3, 6);
    for (char32_t ch : {U'a', U'b', U'c', U'd'}) {
        g.putChar(ch);
    }
    g.putChar(U'e');
    g.putChar(0x0301);
    const char32_t stored = g.cellAt(0, 4).ch;
    REQUIRE(g.clusters().lookup(stored).size() == 2);

    g.resize(3, 3);

    // Moved to a new row, still resolving to the same two codepoints — the
    // cluster rode along inside the Cell, which is the whole point of the
    // tagged encoding.
    CHECK(g.cellAt(1, 1).ch == stored);
    CHECK(g.clusters().lookup(g.cellAt(1, 1).ch).size() == 2);
}
