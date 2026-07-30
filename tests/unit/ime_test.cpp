// T29 — IME composition width and candidate-window placement.
//
// rules/render.md: "composition string + candidate window positioning comes
// from the renderer's cell metrics — any metrics change re-runs the IME
// positioning tests (Thai + Japanese)". This file IS those tests, and it needs
// no window: the metrics are the input.
//
// The two scripts are not decoration. Thai composes with zero-width marks and
// Japanese with double-width characters, so an implementation that counts
// CHARACTERS instead of measuring cells gets the candidate window wrong in
// opposite directions — and passes a Latin-only test either way.

#include "app/input/ime.h"
#include "render/ime_metrics.h"
#include <catch2/catch_test_macros.hpp>

using krait::app::input::Composition;
using krait::core::unicode::Ambiguous;
using krait::render::cursorRect;
using krait::render::FaceMetrics;
using krait::render::preeditCells;
using krait::render::preeditRect;

namespace {

// A plausible monospace cell: 12x23 device pixels, which is what Cascadia Mono
// at 20px measures on the dev machine.
FaceMetrics metrics() {
    FaceMetrics m;
    m.cellWidth = 12;
    m.lineHeight = 23;
    m.ascent = 18;
    m.descent = 5;
    return m;
}

}  // namespace

TEST_CASE("the cursor rect follows the cell metrics", "[ime]") {
    const auto rect = cursorRect(metrics(), 3, 5, 80, 24);
    CHECK(rect.x == 5 * 12);
    CHECK(rect.y == 3 * 23);
    CHECK(rect.w == 12);
    CHECK(rect.h == 23);
}

TEST_CASE("a doubled cell size moves the candidate window with it", "[ime]") {
    // This is the DPI case: the font is re-rasterised at 2x and the candidate
    // window has to follow, or the IME draws its list over the wrong text.
    FaceMetrics big = metrics();
    big.cellWidth = 24;
    big.lineHeight = 46;
    const auto rect = cursorRect(big, 3, 5, 80, 24);
    CHECK(rect.x == 5 * 24);
    CHECK(rect.y == 3 * 46);
    CHECK(rect.w == 24);
    CHECK(rect.h == 46);
}

TEST_CASE("a rect past the edge is clamped inside the widget", "[ime]") {
    // An IME handed a rectangle outside the widget puts its candidate list
    // off-screen, or on whichever monitor owns that coordinate. A composition
    // that grows past the right edge is exactly when that happens.
    const auto rect = cursorRect(metrics(), 99, 200, 80, 24);
    CHECK(rect.x == 79 * 12);
    CHECK(rect.y == 23 * 23);
    // Negative coordinates come from a cursor that has not been placed yet.
    const auto negative = cursorRect(metrics(), -4, -9, 80, 24);
    CHECK(negative.x == 0);
    CHECK(negative.y == 0);
}

TEST_CASE("a preedit is truncated at the row edge, never wrapped", "[ime]") {
    // The grid does not own these cells: there is no next row to continue onto,
    // and drawing past the edge paints over the chrome.
    CHECK(preeditCells(70, 5, 80) == 5);
    CHECK(preeditCells(78, 5, 80) == 2);
    CHECK(preeditCells(79, 5, 80) == 1);
    CHECK(preeditCells(0, 0, 80) == 0);

    const auto rect = preeditRect(metrics(), 1, 78, 5, 80, 24);
    CHECK(rect.x == 78 * 12);
    CHECK(rect.w == 2 * 12);
}

TEST_CASE("Thai composition measures in cells, not characters", "[ime]") {
    // สวัสดี is 6 UTF-16 units but only 4 display cells: the vowel sign and the
    // tone mark are zero width. Counting characters would put the candidate
    // window two cells too far right, over text the user is still reading.
    Composition composition;
    composition.setPreedit(QString::fromUtf8("สวัสดี"), 6);
    CHECK(composition.active());
    CHECK(composition.text().size() == 6);
    CHECK(composition.clusters().size() == 4);
    CHECK(composition.columns(Ambiguous::Narrow) == 4);
    CHECK(composition.cursorColumns(Ambiguous::Narrow) == 4);
}

TEST_CASE("a partial Thai composition tracks the caret in cells", "[ime]") {
    Composition composition;
    // Caret after "สวั" — 3 UTF-16 units, 2 clusters, 2 cells.
    composition.setPreedit(QString::fromUtf8("สวัสดี"), 3);
    CHECK(composition.cursorColumns(Ambiguous::Narrow) == 2);
}

TEST_CASE("Japanese composition is double width", "[ime]") {
    // へんかん is 4 characters and 8 cells. A character count would put the
    // candidate window four cells too far LEFT — the opposite error to Thai,
    // which is why one script is not enough coverage.
    Composition composition;
    composition.setPreedit(QString::fromUtf8("へんかん"), 4);
    CHECK(composition.clusters().size() == 4);
    CHECK(composition.columns(Ambiguous::Narrow) == 8);
    CHECK(composition.cursorColumns(Ambiguous::Narrow) == 8);

    // Mid-composition, after the candidate window converted two of them.
    composition.setPreedit(QString::fromUtf8("変換かん"), 2);
    CHECK(composition.columns(Ambiguous::Narrow) == 8);
    CHECK(composition.cursorColumns(Ambiguous::Narrow) == 4);
}

TEST_CASE("a composition above the BMP is one cluster, not two halves", "[ime]") {
    // A QString is UTF-16, so an emoji is a surrogate PAIR. Fed to the cluster
    // breaker one half at a time it segments as two broken codepoints and
    // measures wrong.
    Composition composition;
    composition.setPreedit(QString::fromUtf8("😀"), 2);
    CHECK(composition.clusters().size() == 1);
    CHECK(composition.columns(Ambiguous::Narrow) == 2);
}

TEST_CASE("clearing ends the composition", "[ime]") {
    Composition composition;
    composition.setPreedit(QString::fromUtf8("へん"), 2);
    REQUIRE(composition.active());
    composition.clear();
    CHECK_FALSE(composition.active());
    CHECK(composition.columns(Ambiguous::Narrow) == 0);
    CHECK(composition.text().isEmpty());
}

TEST_CASE("an empty preedit is not an active composition", "[ime]") {
    // The IME sends an empty preedit to cancel; treating that as active leaves
    // an underline on screen with nothing under it.
    Composition composition;
    composition.setPreedit(QString(), 0);
    CHECK_FALSE(composition.active());
}
