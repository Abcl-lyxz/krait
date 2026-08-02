#include "core/grid/grid.h"
#include "render/frame_builder.h"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using krait::core::vt::Attr;
using krait::core::vt::Color;
using krait::core::vt::DamageList;
using krait::core::vt::Line;
using krait::core::vt::Underline;
using krait::render::CursorStyle;
using krait::render::FaceMetrics;
using krait::render::FrameBuilder;
using krait::render::FrameParams;
using krait::render::GlyphAtlas;
using krait::render::GlyphBitmap;
using krait::render::paletteColor;
using krait::render::resolveColors;
using krait::render::Selection;
using krait::render::selectionContains;
using krait::render::Theme;

namespace {

constexpr FaceMetrics kMetrics{.cellWidth = 10, .ascent = 16, .descent = 4, .lineHeight = 20};

krait::render::RasterFn inkRaster() {
    return [](std::uint32_t, std::uint32_t, GlyphBitmap& out) {
        out.width = 8;
        out.height = 12;
        out.bearingX = 1;
        out.bearingY = 12;
        out.gray.assign(static_cast<std::size_t>(out.width) * out.height, 0xFF);
        return true;
    };
}

// No shaped runs: enough for the tests that only care about backgrounds,
// selection and the cursor.
FrameBuilder::RowRuns noRuns() {
    return FrameBuilder::RowRuns{};
}

}  // namespace

TEST_CASE("the xterm 256-colour palette uses the documented level table", "[frame]") {
    const Theme theme;
    // The cube's levels are 0, 95, 135, 175, 215, 255 — NOT index * 51, which is
    // the classic error when this is derived arithmetically.
    CHECK(paletteColor(16, theme) == 0x000000);
    CHECK(paletteColor(21, theme) == 0x0000FF);
    CHECK(paletteColor(231, theme) == 0xFFFFFF);
    CHECK(paletteColor(196, theme) == 0xFF0000);
    // Greyscale ramp, 8 to 238.
    CHECK(paletteColor(232, theme) == 0x080808);
    CHECK(paletteColor(255, theme) == 0xEEEEEE);
    // 0-15 come from the theme, so they are not hardcoded here.
    CHECK(paletteColor(1, theme) == theme.ansi[1]);
}

TEST_CASE("cell colours honour reverse, bold, dim and invisible", "[frame]") {
    const Theme theme;
    std::uint32_t fg = 0;
    std::uint32_t bg = 0;

    SECTION("defaults come from the theme") {
        resolveColors(Attr{}, theme, fg, bg);
        CHECK(fg == theme.fg);
        CHECK(bg == theme.bg);
    }

    SECTION("reverse swaps them") {
        Attr attr;
        attr.flags = Attr::kReverse;
        resolveColors(attr, theme, fg, bg);
        CHECK(fg == theme.bg);
        CHECK(bg == theme.fg);
    }

    SECTION("bold brightens an indexed colour into the high half") {
        Attr attr;
        attr.flags = Attr::kBold;
        attr.fg = Color::indexed(1);
        resolveColors(attr, theme, fg, bg);
        CHECK(fg == theme.ansi[9]);
    }

    SECTION("bold does NOT touch a truecolor foreground") {
        // An application that asked for an exact RGB has no way to opt out of
        // brightening, so brightening it would be a bug, not a feature.
        Attr attr;
        attr.flags = Attr::kBold;
        attr.fg = Color::rgb(0x123456);
        resolveColors(attr, theme, fg, bg);
        CHECK(fg == 0x123456);
    }

    SECTION("invisible wins over reverse") {
        Attr attr;
        attr.flags = Attr::kReverse | Attr::kInvisible;
        attr.fg = Color::rgb(0xFFFFFF);
        attr.bg = Color::rgb(0x000000);
        resolveColors(attr, theme, fg, bg);
        CHECK(fg == bg);  // SGR 7 then SGR 8 still hides the text
    }
}

TEST_CASE("selection covers the right cells in both drag directions", "[frame]") {
    SECTION("a forward single-row drag") {
        const Selection sel{
            .active = true, .anchorRow = 1, .anchorCol = 2, .cursorRow = 1, .cursorCol = 5};
        CHECK_FALSE(selectionContains(sel, 1, 1));
        CHECK(selectionContains(sel, 1, 2));
        CHECK(selectionContains(sel, 1, 5));
        CHECK_FALSE(selectionContains(sel, 1, 6));
        CHECK_FALSE(selectionContains(sel, 0, 3));
    }

    SECTION("dragging upwards selects the same cells") {
        const Selection down{
            .active = true, .anchorRow = 0, .anchorCol = 3, .cursorRow = 2, .cursorCol = 4};
        const Selection up{
            .active = true, .anchorRow = 2, .anchorCol = 4, .cursorRow = 0, .cursorCol = 3};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 8; ++col) {
                CHECK(selectionContains(down, row, col) == selectionContains(up, row, col));
            }
        }
    }

    SECTION("a middle row is selected end to end") {
        const Selection sel{
            .active = true, .anchorRow = 0, .anchorCol = 5, .cursorRow = 2, .cursorCol = 1};
        CHECK(selectionContains(sel, 1, 0));
        CHECK(selectionContains(sel, 1, 99));
        CHECK_FALSE(selectionContains(sel, 0, 4));  // before the anchor
        CHECK(selectionContains(sel, 2, 1));
        CHECK_FALSE(selectionContains(sel, 2, 2));  // past the cursor
    }

    SECTION("an inactive selection contains nothing") {
        const Selection sel{
            .active = false, .anchorRow = 0, .anchorCol = 0, .cursorRow = 9, .cursorCol = 9};
        CHECK_FALSE(selectionContains(sel, 3, 3));
    }
}

TEST_CASE("a trigger highlight becomes a solid the GPU can draw", "[frame]") {
    // T68. The highlight action has to reach the renderer, and this is the
    // seam: spans in viewport coordinates become rectangles, clipped to the
    // grid, in the same pipeline the selection uses.
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    std::vector<Line> viewport(3, Line(20));
    DamageList damage(3);
    FrameParams params;
    params.cols = 20;
    params.cursor.visible = false;

    builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
    const std::size_t baseline = builder.solids().size();

    const std::array<krait::render::HighlightSpan, 4> spans{{
        {.row = 1, .beginCol = 2, .endCol = 6},
        {.row = 9, .beginCol = 0, .endCol = 4},    // off the bottom: dropped
        {.row = 0, .beginCol = 5, .endCol = 5},    // empty: dropped
        {.row = 2, .beginCol = 18, .endCol = 99},  // clipped to the grid width
    }};
    params.highlights = spans;
    damage.markAll();
    builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
    REQUIRE(builder.solids().size() == baseline + 2);

    const auto& first = builder.solids()[baseline];
    CHECK(first.x == 2.0F * kMetrics.cellWidth);
    CHECK(first.y == 1.0F * kMetrics.lineHeight);
    CHECK(first.w == 4.0F * kMetrics.cellWidth);
    // Semi-transparent, so the glyphs underneath stay legible without a second
    // text pass in a highlight foreground colour.
    CHECK(first.a < 1.0F);

    // Clipped rather than dropped: a match running to the end of the row is the
    // normal case, and a rectangle past the last column would be drawn outside
    // the grid.
    CHECK(builder.solids()[baseline + 1].w == 2.0F * kMetrics.cellWidth);
}

TEST_CASE("only damaged rows are rebuilt", "[frame]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    std::vector<Line> viewport(5, Line(20));
    DamageList damage(5);
    FrameParams params;
    params.cols = 20;
    params.cursor.visible = false;

    // First frame: everything is new, so every row must be built.
    builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
    CHECK(builder.rowsRebuilt() == 5);

    SECTION("a clean frame rebuilds nothing") {
        builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
        // This is the rules/render.md rule made checkable: a full-frame redraw
        // outside resize or theme change is a defect, not just a slow path.
        CHECK(builder.rowsRebuilt() == 0);
    }

    SECTION("one damaged row rebuilds exactly one row") {
        damage.mark(2, 0, 19);
        builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
        CHECK(builder.rowsRebuilt() == 1);
    }

    SECTION("markAll rebuilds everything") {
        damage.markAll();
        builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
        CHECK(builder.rowsRebuilt() == 5);
    }

    SECTION("a theme change is a legitimate full redraw") {
        Theme dark;
        dark.bg = 0x000000;
        builder.setTheme(dark);
        builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
        CHECK(builder.rowsRebuilt() == 5);
    }

    SECTION("a resize is too") {
        std::vector<Line> taller(7, Line(20));
        builder.build(taller, damage, params, raster, atlas, [](int) { return noRuns(); });
        CHECK(builder.rowsRebuilt() == 7);
    }

    SECTION("the cursor moves without rebuilding any row") {
        params.cursor.visible = true;
        params.cursor.row = 3;
        params.cursor.col = 4;
        builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
        CHECK(builder.rowsRebuilt() == 0);  // the cursor is not row-cached
        CHECK_FALSE(builder.solids().empty());
    }
}

TEST_CASE("each cursor style emits its own geometry", "[frame]") {
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();
    const std::vector<Line> viewport(3, Line(10));
    const DamageList damage(3);

    const auto solidsFor = [&](CursorStyle style, bool focused) {
        FrameBuilder builder(kMetrics, Theme{});
        FrameParams params;
        params.cols = 10;
        params.cursor = {.visible = true, .focused = focused, .row = 1, .col = 2, .style = style};
        builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
        return std::vector<krait::render::SolidInstance>(builder.solids().begin(),
                                                         builder.solids().end());
    };

    SECTION("a block fills the whole cell") {
        const auto solids = solidsFor(CursorStyle::Block, true);
        REQUIRE(solids.size() == 1);
        CHECK(solids[0].x == 20.0F);  // col 2 * cellWidth 10
        CHECK(solids[0].y == 20.0F);  // row 1 * lineHeight 20
        CHECK(solids[0].w == 10.0F);
        CHECK(solids[0].h == 20.0F);
    }

    SECTION("a bar is thin and full height") {
        const auto solids = solidsFor(CursorStyle::Bar, true);
        REQUIRE(solids.size() == 1);
        CHECK(solids[0].w < 10.0F);
        CHECK(solids[0].h == 20.0F);
    }

    SECTION("an underline is thin and sits at the bottom of the cell") {
        const auto solids = solidsFor(CursorStyle::Underline, true);
        REQUIRE(solids.size() == 1);
        CHECK(solids[0].w == 10.0F);
        CHECK(solids[0].h < 20.0F);
        CHECK(solids[0].y + solids[0].h == 40.0F);  // flush with the cell bottom
    }

    SECTION("losing focus turns a block into an outline of four edges") {
        const auto solids = solidsFor(CursorStyle::Block, false);
        // The only cue that keystrokes are going somewhere else.
        CHECK(solids.size() == 4);
    }

    SECTION("an invisible cursor emits nothing") {
        FrameBuilder builder(kMetrics, Theme{});
        FrameParams params;
        params.cols = 10;
        params.cursor = {.visible = false, .focused = true, .row = 1, .col = 2};
        builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
        CHECK(builder.solids().empty());
    }

    SECTION("a cursor past the last viewport row is dropped, not drawn off-screen") {
        FrameBuilder builder(kMetrics, Theme{});
        FrameParams params;
        params.cols = 10;
        params.cursor = {.visible = true, .focused = true, .row = 99, .col = 2};
        builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
        CHECK(builder.solids().empty());
    }
}

TEST_CASE("copying a wide cluster does not emit its trailing half", "[frame][width]") {
    // REGRESSION. A double-width cluster stores kWideTrailing (0x80000000) in
    // its right-hand cell. That value is not a cluster ref, so lookup() returns
    // an empty span, and the literal-codepoint arm used to encode 0x80000000 as
    // UTF-8 — four garbage bytes on the clipboard after every CJK or emoji
    // character a selection crossed. It has to contribute nothing at all: not a
    // codepoint, and not a space either, or "日本" would paste as "日 本 ".
    krait::core::vt::ClusterPool pool;
    Line line(6);
    line.cells[0].ch = U'日';  // 日, two columns
    line.cells[1].ch = krait::core::vt::kWideTrailing;
    line.cells[2].ch = U'本';  // 本, two columns
    line.cells[3].ch = krait::core::vt::kWideTrailing;
    const std::array<Line, 1> viewport{line};

    const Selection all{
        .active = true, .anchorRow = 0, .anchorCol = 0, .cursorRow = 0, .cursorCol = 5};
    CHECK(krait::render::selectionText(viewport, all, pool) ==
          std::string("\xe6\x97\xa5\xe6\x9c\xac"));
}

TEST_CASE("selection emits one rect per contiguous span, not per cell", "[frame]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();
    const std::vector<Line> viewport(3, Line(20));
    const DamageList damage(3);

    FrameParams params;
    params.cols = 20;
    params.cursor.visible = false;
    params.selection = {
        .active = true, .anchorRow = 0, .anchorCol = 4, .cursorRow = 0, .cursorCol = 9};

    builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
    REQUIRE(builder.solids().size() == 1);
    const auto& rect = builder.solids()[0];
    CHECK(rect.x == 40.0F);  // col 4
    CHECK(rect.w == 60.0F);  // 6 cells, 4 through 9 inclusive
    CHECK(rect.h == 20.0F);
    CHECK(rect.a < 1.0F);  // translucent so the text under it stays legible
}

TEST_CASE("a non-default background emits one coalesced rect for the span", "[frame]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    std::vector<Line> viewport(1, Line(10));
    for (int col = 2; col < 7; ++col) {
        viewport[0].cells[static_cast<std::size_t>(col)].attr.bg = Color::rgb(0x804020);
        viewport[0].cells[static_cast<std::size_t>(col)].ch = U'x';
    }
    DamageList damage(1);
    damage.markAll();
    FrameParams params;
    params.cols = 10;
    params.cursor.visible = false;

    builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
    // Five cells of identical background must be ONE rect, not five.
    REQUIRE(builder.solids().size() == 1);
    CHECK(builder.solids()[0].x == 20.0F);
    CHECK(builder.solids()[0].w == 50.0F);
}

TEST_CASE("glyphs land in their cluster's cell, marks included", "[frame]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    // Two clusters, the second at column 1; the second glyph is a mark with a
    // zero advance that must still be placed over column 1.
    krait::render::Run run;
    run.text = U"ab";
    run.clusters = {krait::render::ClusterRef{.col = 0, .cells = 1, .len = 1, .offset = 0},
                    krait::render::ClusterRef{.col = 1, .cells = 1, .len = 1, .offset = 1}};
    krait::render::ShapedRun shaped;
    shaped.glyphs = {krait::render::ShapedGlyph{.glyphId = 5, .xAdvance = 640, .cluster = 0},
                     krait::render::ShapedGlyph{.glyphId = 6, .xAdvance = 0, .cluster = 1}};
    const std::vector<krait::render::Run> runs{run};
    const std::vector<krait::render::ShapedRun> shapedRuns{shaped};
    const std::vector<std::uint32_t> faces{0};

    std::vector<Line> viewport(1, Line(10));
    for (int col = 0; col < 2; ++col) {
        viewport[0].cells[static_cast<std::size_t>(col)].ch = U'a';
    }
    DamageList damage(1);
    damage.markAll();
    FrameParams params;
    params.cols = 10;
    params.cursor.visible = false;

    builder.build(viewport, damage, params, raster, atlas, [&](int) {
        return FrameBuilder::RowRuns{.runs = runs, .shaped = shapedRuns, .faces = faces};
    });

    REQUIRE(builder.glyphs().size() == 2);
    // Column 0 and column 1, offset by the glyph's left bearing of 1.
    CHECK(builder.glyphs()[0].x == 1.0F);
    CHECK(builder.glyphs()[1].x == 11.0F);
    // Both sit on the same baseline: ascent 16 minus bearingY 12.
    CHECK(builder.glyphs()[0].y == 4.0F);
    CHECK(builder.glyphs()[1].y == 4.0F);
    // UVs are normalised and inside the atlas.
    CHECK(builder.glyphs()[0].u1 > builder.glyphs()[0].u0);
    CHECK(builder.glyphs()[0].u1 <= 1.0F);
    CHECK(builder.glyphs()[0].v1 <= 1.0F);
}

TEST_CASE("underline and strike emit decoration rects inside the row", "[frame]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    std::vector<Line> viewport(1, Line(4));
    viewport[0].cells[0].ch = U'a';
    viewport[0].cells[0].attr.underline = Underline::Single;
    viewport[0].cells[1].ch = U'b';
    viewport[0].cells[1].attr.flags = Attr::kStrike;
    viewport[0].cells[2].ch = U'c';
    viewport[0].cells[2].attr.underline = Underline::Double;

    DamageList damage(1);
    damage.markAll();
    FrameParams params;
    params.cols = 4;
    params.cursor.visible = false;

    builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
    // single + strike + double(2) = 4 rects, all inside the row.
    REQUIRE(builder.solids().size() == 4);
    for (const auto& rect : builder.solids()) {
        CHECK(rect.h >= 1.0F);
        CHECK(rect.y >= 0.0F);
        CHECK(rect.y + rect.h <= 20.0F);  // never bleeds into the next row
    }
}
