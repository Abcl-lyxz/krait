// T71: the copy-mode motion and selection model.
//
// The properties worth asserting are the ones that would make copy mode WRONG
// rather than merely awkward: a motion that comes to rest on the right-hand
// half of a double-width cluster, or inside a multi-codepoint one. `h` working
// is not interesting. `w` over CJK and Thai is the whole reason this file
// exists — CLAUDE.md's first landmine is that wcwidth lies, and a word motion
// that counts codepoints instead of clusters is the same mistake wearing a
// different hat.

#include "core/terminal/session.h"
#include "input/copy_mode.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using krait::app::input::applyMotion;
using krait::app::input::Command;
using krait::app::input::CopyCursor;
using krait::app::input::Motion;
using krait::app::input::Selecting;
using krait::app::input::translateCopyKey;
using krait::core::vt::Session;

namespace {

void feed(Session& session, std::string_view text) {
    session.feed({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

// A viewport built the way TerminalItem builds it, so the tests run against the
// same rows the renderer and the yank path see.
std::vector<krait::core::vt::Line> rows(const Session& session) {
    return session.grid().viewportRows();
}

// Walks `motion` `times` times and reports where the cursor came to rest.
int columnAfter(Session& session, Motion motion, int times, int startCol = 0) {
    CopyCursor cursor{.row = 0, .col = startCol};
    const auto viewport = rows(session);
    for (int i = 0; i < times; ++i) {
        applyMotion(cursor, motion, viewport, session.grid().clusters());
    }
    return cursor.col;
}

}  // namespace

TEST_CASE("a word motion steps over a double-width cluster, not into it", "[copymode][width]") {
    // Each CJK character owns TWO columns and the right-hand one holds
    // kWideTrailing — it is not a position a cursor may occupy. A `w` that
    // counted codepoints would land on column 3; the columns are 0,2,4 and the
    // next word starts at 6.
    Session session(4, 40);
    feed(session, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e text\r\n");

    CopyCursor cursor{.row = 0, .col = 0};
    const auto viewport = rows(session);
    applyMotion(cursor, Motion::WordNext, viewport, session.grid().clusters());

    // Past the three wide clusters (columns 0, 2, 4) and the space at 6.
    CHECK(cursor.col == 7);
}

TEST_CASE("l walks whole clusters across double-width text", "[copymode][width]") {
    Session session(4, 40);
    feed(session, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\r\n");

    // Two columns a step, never landing on a trailing half.
    CHECK(columnAfter(session, Motion::Right, 1) == 2);
    CHECK(columnAfter(session, Motion::Right, 2) == 4);
    // And back the same way: a naive col-1 would stop on 3 and 1.
    CHECK(columnAfter(session, Motion::Left, 1, 4) == 2);
    CHECK(columnAfter(session, Motion::Left, 2, 4) == 0);
}

TEST_CASE("a word motion treats a Thai word as one word", "[copymode][thai]") {
    // "สวัสดี ครับ" — two space-separated words whose characters carry combining
    // marks, so several codepoints share one cell. Landing anywhere but the
    // first cell of the second word means the motion counted something other
    // than clusters.
    Session session(4, 40);
    feed(session, "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5"
                  " \xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb8\x9a\r\n");

    CopyCursor cursor{.row = 0, .col = 0};
    const auto viewport = rows(session);
    applyMotion(cursor, Motion::WordNext, viewport, session.grid().clusters());

    // "สวัสดี" is four CELLS (ส ว+ั ส ด+ี), then the space, so the second word
    // starts at column 5. The exact number matters less than that it is a
    // cluster start and past the space.
    CHECK(cursor.col == 5);
    CHECK(cursor.row == 0);
}

TEST_CASE("a vertical motion never lands on a trailing half", "[copymode][width]") {
    // THE case a per-arm snap would miss: the column survives a row change, and
    // the new row can have a wide cluster exactly where the old one had a
    // boundary. Row 0 is ASCII, so column 3 is a cluster start there; row 1 is
    // CJK, where column 3 is the trailing half of the cluster at 2.
    Session session(4, 40);
    feed(session, "abcdef\r\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\r\n");

    CopyCursor cursor{.row = 0, .col = 3};
    const auto viewport = rows(session);
    applyMotion(cursor, Motion::Down, viewport, session.grid().clusters());

    CHECK(cursor.row == 1);
    CHECK(cursor.col == 2);  // snapped back to the cluster that owns column 3
}

TEST_CASE("w stops at punctuation the way vim does", "[copymode]") {
    Session session(4, 40);
    feed(session, "path/to file\r\n");

    CHECK(columnAfter(session, Motion::WordNext, 1) == 4);  // the '/'
    CHECK(columnAfter(session, Motion::WordNext, 2) == 5);  // "to"
    CHECK(columnAfter(session, Motion::WordNext, 3) == 8);  // "file"
}

TEST_CASE("b and e are w's inverses over the same text", "[copymode]") {
    Session session(4, 40);
    feed(session, "alpha beta gamma\r\n");

    CHECK(columnAfter(session, Motion::WordNext, 2) == 11);  // "gamma"
    CHECK(columnAfter(session, Motion::WordPrev, 1, 11) == 6);
    CHECK(columnAfter(session, Motion::WordPrev, 2, 11) == 0);
    // `e` lands on the LAST cell of a word, not past it.
    CHECK(columnAfter(session, Motion::WordEnd, 1) == 4);
    CHECK(columnAfter(session, Motion::WordEnd, 2) == 9);
}

TEST_CASE("dollar lands on written text, not on the padding", "[copymode]") {
    // Every row is allocated to the full width. `$` landing at column 39 of a
    // six-character line would put the cursor in blank padding and make a
    // v-then-$ selection copy a screen's worth of spaces.
    Session session(4, 40);
    feed(session, "abcdef\r\n");

    CHECK(columnAfter(session, Motion::LineEnd, 1, 2) == 5);
    CHECK(columnAfter(session, Motion::LineStart, 1, 5) == 0);
}

TEST_CASE("a motion off the top of the viewport scrolls instead of stopping", "[copymode]") {
    Session session(4, 20);
    for (int i = 0; i < 50; ++i) {
        feed(session, "line\r\n");
    }
    const auto viewport = rows(session);

    CopyCursor cursor{.row = 0, .col = 0};
    // Already at the top row: the cursor cannot go further, so the VIEWPORT is
    // what has to move, and the caller is told by how much.
    CHECK(applyMotion(cursor, Motion::Up, viewport, session.grid().clusters()) == 1);
    CHECK(cursor.row == 0);

    // A row below the top just moves the cursor and asks for no scroll.
    CopyCursor inside{.row = 2, .col = 0};
    CHECK(applyMotion(inside, Motion::Up, viewport, session.grid().clusters()) == 0);
    CHECK(inside.row == 1);
}

TEST_CASE("the anchor follows the cursor until a selection starts", "[copymode]") {
    Session session(4, 40);
    feed(session, "alpha beta\r\n");
    const auto viewport = rows(session);

    CopyCursor cursor{.row = 0, .col = 0};
    applyMotion(cursor, Motion::WordNext, viewport, session.grid().clusters());
    // No selection yet, so there is nothing to drag: the anchor stays under the
    // cursor. Otherwise pressing `v` after moving would select everything
    // walked over on the way there.
    CHECK(cursor.anchorCol == cursor.col);

    cursor.select = Selecting::Char;
    const int from = cursor.col;
    applyMotion(cursor, Motion::WordNext, viewport, session.grid().clusters());
    CHECK(cursor.anchorCol == from);
    CHECK(cursor.col != from);
}

TEST_CASE("copy mode claims its keys and leaves the chrome's alone", "[copymode]") {
    CHECK(translateCopyKey(Qt::Key_H, Qt::NoModifier).kind == Command::Kind::Move);
    CHECK(translateCopyKey(Qt::Key_H, Qt::NoModifier).motion == Motion::Left);
    CHECK(translateCopyKey(Qt::Key_Y, Qt::NoModifier).kind == Command::Kind::Yank);
    CHECK(translateCopyKey(Qt::Key_Escape, Qt::NoModifier).kind == Command::Kind::Leave);
    CHECK(translateCopyKey(Qt::Key_V, Qt::NoModifier).select == Selecting::Char);
    CHECK(translateCopyKey(Qt::Key_V, Qt::ShiftModifier).select == Selecting::Line);
    // g and G differ only by shift, and Qt reports Key_G for both.
    CHECK(translateCopyKey(Qt::Key_G, Qt::NoModifier).motion == Motion::Top);
    CHECK(translateCopyKey(Qt::Key_G, Qt::ShiftModifier).motion == Motion::Bottom);
    CHECK(translateCopyKey(Qt::Key_U, Qt::ControlModifier).motion == Motion::HalfPageUp);

    // NOT ours. A mode that swallowed the palette shortcut would be a trap the
    // user cannot get out of without knowing Escape.
    CHECK(translateCopyKey(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier).kind ==
          Command::Kind::None);
    CHECK(translateCopyKey(Qt::Key_F1, Qt::NoModifier).kind == Command::Kind::None);
}
