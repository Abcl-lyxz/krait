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

// --- T84: graphics placements ---

namespace {

// A viewport of blank lines plus the parallel stable-index vector a real caller
// derives by walking them. No wrapped rows here, so it is one index per row.
struct ImageFixture {
    std::vector<Line> viewport;
    std::vector<std::uint64_t> rowStable;
    krait::core::vt::ImageStore store;

    ImageFixture(int rows, int cols, std::uint64_t topStable) {
        for (int r = 0; r < rows; ++r) {
            viewport.emplace_back(cols);
            rowStable.push_back(topStable + static_cast<std::uint64_t>(r));
        }
    }

    void addImage(std::uint32_t id) {
        krait::core::vt::Image image;
        image.width = 4;
        image.height = 4;
        image.pixels.assign(16, 0xFF0000FFU);
        store.put(id, std::move(image));
    }

    FrameParams params() const {
        FrameParams out;
        out.cols = 8;
        out.cursor.visible = false;
        out.placements = store.placements();
        out.rowStable = rowStable;
        out.images = &store;
        return out;
    }
};

}  // namespace

TEST_CASE("frame: a placement draws at the viewport row its anchor names", "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    ImageFixture fixture(4, 8, 100);
    fixture.addImage(7);
    REQUIRE(fixture.store.place({.imageId = 7, .anchor = 102, .col = 3, .cols = 2, .rows = 2}));

    DamageList damage(4);
    damage.markAll();
    const FrameParams params = fixture.params();
    builder.build(fixture.viewport, damage, params, raster, atlas, [](int) { return noRuns(); });

    REQUIRE(builder.images().size() == 1);
    const auto& quad = builder.images()[0];
    // anchor 102 is viewport row 2 -> y = 2 * lineHeight; col 3 -> x = 3 * cellWidth.
    CHECK(quad.x == 30.0F);
    CHECK(quad.y == 40.0F);
    CHECK(quad.w == 20.0F);  // 2 cells wide
    CHECK(quad.h == 40.0F);  // 2 rows tall
    // srcW/srcH left at 0 means the whole image, so the full uv range.
    CHECK(quad.u0 == 0.0F);
    CHECK(quad.v0 == 0.0F);
    CHECK(quad.u1 == 1.0F);
    CHECK(quad.v1 == 1.0F);
}

TEST_CASE("frame: a placement scrolled out of the viewport draws nothing", "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    ImageFixture fixture(4, 8, 100);
    fixture.addImage(1);
    // Anchored above the viewport, whose rows are 100..103.
    REQUIRE(fixture.store.place({.imageId = 1, .anchor = 40, .cols = 2, .rows = 2}));

    DamageList damage(4);
    damage.markAll();
    const FrameParams params = fixture.params();
    builder.build(fixture.viewport, damage, params, raster, atlas, [](int) { return noRuns(); });

    CHECK(builder.images().empty());
    CHECK(builder.imageBatches().empty());
}

TEST_CASE("frame: a negative zIndex batches before the text and a positive one after",
          "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    ImageFixture fixture(4, 8, 0);
    fixture.addImage(1);
    fixture.addImage(2);
    // Transmitted over-first, so the sort has something to do.
    REQUIRE(fixture.store.place({.imageId = 2, .anchor = 1, .cols = 1, .rows = 1, .zIndex = 5}));
    REQUIRE(fixture.store.place({.imageId = 1, .anchor = 0, .cols = 1, .rows = 1, .zIndex = -1}));

    DamageList damage(4);
    damage.markAll();
    const FrameParams params = fixture.params();
    builder.build(fixture.viewport, damage, params, raster, atlas, [](int) { return noRuns(); });

    REQUIRE(builder.images().size() == 2);
    REQUIRE(builder.imageBatches().size() == 2);
    // The watermark sorts first and is the only batch drawn under the glyphs.
    CHECK(builder.belowBatchCount() == 1);
    CHECK(builder.imageBatches()[0].imageId == 1);
    CHECK(builder.imageBatches()[1].imageId == 2);
}

TEST_CASE("frame: several placements of one image share a batch", "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    ImageFixture fixture(4, 8, 0);
    fixture.addImage(9);
    // The kitty a=p case: one transmission, placed three times.
    for (std::uint64_t row = 0; row < 3; ++row) {
        REQUIRE(fixture.store.place({.imageId = 9, .anchor = row, .cols = 1, .rows = 1}));
    }

    DamageList damage(4);
    damage.markAll();
    const FrameParams params = fixture.params();
    builder.build(fixture.viewport, damage, params, raster, atlas, [](int) { return noRuns(); });

    CHECK(builder.images().size() == 3);
    // One texture, so one draw call rather than three.
    REQUIRE(builder.imageBatches().size() == 1);
    CHECK(builder.imageBatches()[0].count == 3);
    CHECK(builder.belowBatchCount() == 0);  // zIndex 0 draws over the text
}

TEST_CASE("frame: the number of distinct textures a frame draws is bounded", "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    // 200 distinct images, all anchored inside the viewport. ImageStore evicts
    // by BYTES and these are 4x4, so nothing there evicts them — the shape a
    // hostile stream produces with transmit-then-place in a loop.
    ImageFixture fixture(4, 8, 0);
    for (std::uint32_t id = 1; id <= 200; ++id) {
        fixture.addImage(id);
        REQUIRE(fixture.store.place({.imageId = id, .anchor = id % 4, .cols = 1, .rows = 1}));
    }

    DamageList damage(4);
    damage.markAll();
    const FrameParams params = fixture.params();
    builder.build(fixture.viewport, damage, params, raster, atlas, [](int) { return noRuns(); });

    // Must not exceed what GpuResources will hold textures for, or every frame
    // evicts the textures that same frame still needs.
    CHECK(builder.imageBatches().size() <= 64);
    CHECK(builder.images().size() <= 256);
}

TEST_CASE("frame: a placement whose pixels were evicted draws nothing", "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    ImageFixture fixture(4, 8, 0);
    fixture.addImage(3);
    REQUIRE(fixture.store.place({.imageId = 3, .anchor = 1, .cols = 2, .rows = 2}));
    fixture.store.erase(3);  // the image goes, and its placements with it

    DamageList damage(4);
    damage.markAll();
    const FrameParams params = fixture.params();
    builder.build(fixture.viewport, damage, params, raster, atlas, [](int) { return noRuns(); });

    CHECK(builder.images().empty());
}

TEST_CASE("frame: a source rectangle normalises against the image size", "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    ImageFixture fixture(4, 8, 0);
    fixture.addImage(5);  // 4x4 pixels
    // The bottom-right quadrant of it.
    REQUIRE(fixture.store.place({.imageId = 5,
                                 .anchor = 0,
                                 .cols = 1,
                                 .rows = 1,
                                 .srcX = 2,
                                 .srcY = 2,
                                 .srcW = 2,
                                 .srcH = 2}));

    DamageList damage(4);
    damage.markAll();
    const FrameParams params = fixture.params();
    builder.build(fixture.viewport, damage, params, raster, atlas, [](int) { return noRuns(); });

    REQUIRE(builder.images().size() == 1);
    const auto& quad = builder.images()[0];
    CHECK(quad.u0 == 0.5F);
    CHECK(quad.v0 == 0.5F);
    CHECK(quad.u1 == 1.0F);
    CHECK(quad.v1 == 1.0F);
}

TEST_CASE("frame: a scaled run draws from the SIZED atlas, not the main one", "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    // What terminal_item builds: sized for the protocol's largest scale.
    GlyphAtlas sized(kMetrics.cellWidth * 7, kMetrics.lineHeight * 7);
    const auto raster = inkRaster();

    std::vector<Line> viewport;
    viewport.emplace_back(4);
    viewport[0].cells[0].ch = U'A';
    viewport[0].cells[0].attr.setScale(2);

    std::vector<krait::render::Run> runs(1);
    runs[0].row = 0;
    runs[0].col = 0;
    runs[0].scale = 2;
    runs[0].text = U"A";
    runs[0].clusters.push_back(krait::render::ClusterRef{.col = 0, .cells = 1, .len = 1});
    std::vector<krait::render::ShapedRun> shaped(1);
    shaped[0].glyphs.push_back(krait::render::ShapedGlyph{.glyphId = 42, .cluster = 0});
    // What shapeWithFallback sets once the scaled re-shape actually succeeded.
    shaped[0].scale = 2;
    const std::vector<std::uint32_t> faces{0};

    DamageList damage(1);
    damage.markAll();
    FrameParams params;
    params.cols = 4;
    params.cursor.visible = false;
    params.sizedAtlas = &sized;

    builder.build(viewport, damage, params, raster, atlas, [&](int) {
        return FrameBuilder::RowRuns{.runs = runs, .shaped = shaped, .faces = faces};
    });

    // The glyph went to the sized list, and the main atlas was never asked.
    CHECK(builder.glyphs().empty());
    REQUIRE(builder.sizedGlyphs().size() == 1);
    CHECK(atlas.residentGlyphs() == 0);
    CHECK(sized.residentGlyphs() == 1);

    // The block hangs DOWN from its row: the baseline is `scale` ascents below
    // the row top, so a 2x glyph sits lower than a 1x one would.
    const auto& inst = builder.sizedGlyphs()[0];
    CHECK(inst.y > static_cast<float>(kMetrics.ascent));
}

TEST_CASE("frame: a scaled run with no sized atlas draws nothing rather than the wrong size",
          "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    std::vector<Line> viewport;
    viewport.emplace_back(4);
    viewport[0].cells[0].ch = U'A';
    viewport[0].cells[0].attr.setScale(3);

    std::vector<krait::render::Run> runs(1);
    runs[0].row = 0;
    runs[0].scale = 3;
    runs[0].text = U"A";
    runs[0].clusters.push_back(krait::render::ClusterRef{.col = 0, .cells = 1, .len = 1});
    std::vector<krait::render::ShapedRun> shaped(1);
    shaped[0].glyphs.push_back(krait::render::ShapedGlyph{.glyphId = 42, .cluster = 0});
    shaped[0].scale = 3;  // the re-shape succeeded; only the atlas is missing
    const std::vector<std::uint32_t> faces{0};

    DamageList damage(1);
    damage.markAll();
    FrameParams params;
    params.cols = 4;
    params.cursor.visible = false;
    params.sizedAtlas = nullptr;  // no sized text was expected

    builder.build(viewport, damage, params, raster, atlas, [&](int) {
        return FrameBuilder::RowRuns{.runs = runs, .shaped = shaped, .faces = faces};
    });

    // Drawing it at 1x out of the main atlas would put a normal-sized glyph
    // where the layout reserved a 3x block, which is worse than a gap.
    CHECK(builder.glyphs().empty());
    CHECK(builder.sizedGlyphs().empty());
}

TEST_CASE("frame: a scaled run whose reshape FAILED draws at 1x from the main atlas",
          "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    GlyphAtlas sized(kMetrics.cellWidth * 7, kMetrics.lineHeight * 7);
    const auto raster = inkRaster();

    std::vector<Line> viewport;
    viewport.emplace_back(4);
    viewport[0].cells[0].ch = U'A';
    viewport[0].cells[0].attr.setScale(4);

    std::vector<krait::render::Run> runs(1);
    runs[0].row = 0;
    runs[0].scale = 4;  // what the cell ASKED for
    runs[0].text = U"A";
    runs[0].clusters.push_back(krait::render::ClusterRef{.col = 0, .cells = 1, .len = 1});
    std::vector<krait::render::ShapedRun> shaped(1);
    shaped[0].glyphs.push_back(krait::render::ShapedGlyph{.glyphId = 42, .cluster = 0});
    // ...and 1 is what actually happened: the face would not load at 4x, or the
    // batch timed out, so shapeWithFallback left the 1x shaping in place.
    shaped[0].scale = 1;
    const std::vector<std::uint32_t> faces{0};

    DamageList damage(1);
    damage.markAll();
    FrameParams params;
    params.cols = 4;
    params.cursor.visible = false;
    params.sizedAtlas = &sized;

    builder.build(viewport, damage, params, raster, atlas, [&](int) {
        return FrameBuilder::RowRuns{.runs = runs, .shaped = shaped, .faces = faces};
    });

    // Routing on run.scale would put this 1x glyph in a scale-7 slot — evicting
    // real sized glyphs — and draw it four rows too low.
    REQUIRE(builder.glyphs().size() == 1);
    CHECK(builder.sizedGlyphs().empty());
    CHECK(atlas.residentGlyphs() == 1);
    CHECK(sized.residentGlyphs() == 0);
    // On the ordinary baseline, not a scaled one.
    CHECK(builder.glyphs()[0].y < static_cast<float>(kMetrics.ascent));
}

TEST_CASE("frame: no image store means no image work at all", "[frame][t84]") {
    FrameBuilder builder(kMetrics, Theme{});
    GlyphAtlas atlas(kMetrics.cellWidth, kMetrics.lineHeight);
    const auto raster = inkRaster();

    std::vector<Line> viewport;
    viewport.emplace_back(4);
    DamageList damage(1);
    damage.markAll();
    FrameParams params;
    params.cols = 4;
    params.cursor.visible = false;

    builder.build(viewport, damage, params, raster, atlas, [](int) { return noRuns(); });
    CHECK(builder.images().empty());
    CHECK(builder.imageBatches().empty());
    CHECK(builder.belowBatchCount() == 0);
}
