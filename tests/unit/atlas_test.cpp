#include "render/atlas/glyph_atlas.h"
#include "render/shaper/fontdb.h"
#include "render/shaper/shape_pool.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using krait::render::AtlasEntry;
using krait::render::FontDb;
using krait::render::GlyphAtlas;
using krait::render::GlyphBitmap;
using krait::render::GlyphKey;
using krait::render::ShapePool;

namespace {

// A rasteriser with no font behind it, so eviction is testable without a GPU,
// a font file, or a particular machine's glyph ids.
krait::render::RasterFn fakeRaster(int width, int height, std::uint8_t value = 0xFF) {
    return [width, height, value](std::uint32_t, std::uint32_t glyphId, GlyphBitmap& out) {
        out.width = width;
        out.height = height;
        out.bearingX = 1;
        out.bearingY = height;
        // Value varies with the glyph so a wrong slot shows up as wrong pixels.
        out.gray.assign(static_cast<std::size_t>(width) * height,
                        static_cast<std::uint8_t>(value ^ (glyphId & 0x7FU)));
        return true;
    };
}

}  // namespace

TEST_CASE("the atlas places a glyph and finds it again", "[atlas]") {
    GlyphAtlas atlas(10, 20);
    const auto raster = fakeRaster(8, 16);

    const AtlasEntry* first = atlas.get(GlyphKey{.faceId = 0, .glyphId = 42}, raster);
    REQUIRE(first != nullptr);
    CHECK(first->width == 8);
    CHECK(first->height == 16);
    CHECK(atlas.residentGlyphs() == 1);
    CHECK(atlas.dirty());

    // A second lookup must not re-rasterise or move the glyph.
    const std::uint16_t x = first->x;
    const std::uint16_t y = first->y;
    const AtlasEntry* again = atlas.get(GlyphKey{.faceId = 0, .glyphId = 42}, raster);
    REQUIRE(again != nullptr);
    CHECK(again->x == x);
    CHECK(again->y == y);
    CHECK(atlas.residentGlyphs() == 1);

    SECTION("the same glyph id in a different face is a different entry") {
        const AtlasEntry* other = atlas.get(GlyphKey{.faceId = 1, .glyphId = 42}, raster);
        REQUIRE(other != nullptr);
        CHECK(atlas.residentGlyphs() == 2);
        CHECK((other->x != x || other->y != y));
    }
}

TEST_CASE("the atlas grows before it evicts, and never past the cap", "[atlas]") {
    GlyphAtlas atlas(10, 20);
    const auto raster = fakeRaster(8, 16);
    const std::size_t initialCapacity = atlas.capacity();
    REQUIRE(initialCapacity > 0);
    CHECK(atlas.height() == GlyphAtlas::kInitialHeight);

    // Fill past the initial capacity: the atlas must GROW, not evict.
    for (std::uint32_t i = 0; i <= initialCapacity; ++i) {
        REQUIRE(atlas.get(GlyphKey{.faceId = 0, .glyphId = i}, raster) != nullptr);
    }
    CHECK(atlas.height() > GlyphAtlas::kInitialHeight);
    CHECK(atlas.evictions() == 0);
    CHECK(atlas.residentGlyphs() == initialCapacity + 1);
    CHECK(atlas.takeGrew());
    CHECK_FALSE(atlas.takeGrew());  // the flag is consumed

    SECTION("growth preserves every already-placed glyph's coordinates") {
        // The whole reason the texture grows in height only. If growth moved a
        // glyph, every atlas coordinate already handed to the GPU would be
        // stale and the screen would show the wrong glyphs.
        const AtlasEntry* zero = atlas.peek(GlyphKey{.faceId = 0, .glyphId = 0});
        REQUIRE(zero != nullptr);
        CHECK(zero->x == GlyphAtlas::kPadding);
        CHECK(zero->y == GlyphAtlas::kPadding);
    }
}

TEST_CASE("the atlas evicts least-recently-used under pressure", "[atlas]") {
    GlyphAtlas atlas(10, 20);
    const auto raster = fakeRaster(8, 16);

    // Fill to the growth cap.
    std::uint32_t next = 0;
    while (atlas.height() < GlyphAtlas::kMaxHeight) {
        REQUIRE(atlas.get(GlyphKey{.faceId = 0, .glyphId = next++}, raster) != nullptr);
    }
    const std::size_t capped = atlas.capacity();
    while (atlas.residentGlyphs() < capped) {
        REQUIRE(atlas.get(GlyphKey{.faceId = 0, .glyphId = next++}, raster) != nullptr);
    }
    REQUIRE(atlas.height() == GlyphAtlas::kMaxHeight);
    REQUIRE(atlas.evictions() == 0);

    // Touch glyph 0 so it is the most recently used, and glyph 1 stays oldest.
    REQUIRE(atlas.get(GlyphKey{.faceId = 0, .glyphId = 0}, raster) != nullptr);

    // One more glyph now has nowhere to go: the atlas is at its cap and full.
    const AtlasEntry* forced = atlas.get(GlyphKey{.faceId = 0, .glyphId = next}, raster);
    REQUIRE(forced != nullptr);

    CHECK(atlas.evictions() == 1);
    CHECK(atlas.height() == GlyphAtlas::kMaxHeight);                    // never past the cap
    CHECK(atlas.residentGlyphs() == capped);                            // bounded, not growing
    CHECK(atlas.peek(GlyphKey{.faceId = 0, .glyphId = 1}) == nullptr);  // the LRU victim
    CHECK(atlas.peek(GlyphKey{.faceId = 0, .glyphId = 0}) != nullptr);  // touched, survived
    CHECK(atlas.peek(GlyphKey{.faceId = 0, .glyphId = next}) != nullptr);

    SECTION("an evicted slot is reused rather than leaked") {
        const std::size_t before = atlas.residentGlyphs();
        for (std::uint32_t i = 1; i <= 50; ++i) {
            REQUIRE(atlas.get(GlyphKey{.faceId = 0, .glyphId = next + i}, raster) != nullptr);
        }
        CHECK(atlas.residentGlyphs() == before);  // still exactly at capacity
        CHECK(atlas.evictions() == 51);
    }
}

TEST_CASE("a glyph too large for a slot is refused, not clipped", "[atlas]") {
    GlyphAtlas atlas(10, 20);
    // Slot is 2*10 + 2 = 22 wide and 20 + 2 = 22 tall.
    CHECK(atlas.get(GlyphKey{.faceId = 0, .glyphId = 1}, fakeRaster(200, 16)) == nullptr);
    CHECK(atlas.get(GlyphKey{.faceId = 0, .glyphId = 2}, fakeRaster(8, 200)) == nullptr);
    CHECK(atlas.residentGlyphs() == 0);
    // Drawing a clipped glyph would be a wrong glyph on screen, which is worse
    // than drawing none — so nothing was stored.
    CHECK(atlas.peek(GlyphKey{.faceId = 0, .glyphId = 1}) == nullptr);
}

TEST_CASE("a rasteriser that fails yields no entry", "[atlas]") {
    GlyphAtlas atlas(10, 20);
    const krait::render::RasterFn failing = [](std::uint32_t, std::uint32_t, GlyphBitmap&) {
        return false;
    };
    CHECK(atlas.get(GlyphKey{.faceId = 0, .glyphId = 7}, failing) == nullptr);
    CHECK(atlas.residentGlyphs() == 0);
}

TEST_CASE("a reused slot's old pixels do not bleed into its replacement", "[atlas]") {
    GlyphAtlas atlas(4, 6);  // small slots so the fill is cheap to verify
    const auto big = fakeRaster(8, 6, 0xFF);
    const auto small = fakeRaster(2, 2, 0x11);

    REQUIRE(atlas.get(GlyphKey{.faceId = 0, .glyphId = 1}, big) != nullptr);
    const AtlasEntry* placed = atlas.peek(GlyphKey{.faceId = 0, .glyphId = 1});
    REQUIRE(placed != nullptr);
    const int slotX = placed->x - GlyphAtlas::kPadding;
    const int slotY = placed->y - GlyphAtlas::kPadding;

    atlas.clear();
    REQUIRE(atlas.get(GlyphKey{.faceId = 0, .glyphId = 2}, small) != nullptr);

    // The tail of the old 8-wide glyph must be gone, or the new 2-wide glyph
    // would render with a stripe of its predecessor beside it.
    const auto& pixels = atlas.pixels();
    const auto row = static_cast<std::size_t>(slotY + GlyphAtlas::kPadding) * GlyphAtlas::kWidth;
    CHECK(pixels[row + static_cast<std::size_t>(slotX) + 6] == 0);
}

TEST_CASE("the atlas rasterises real FreeType glyphs through the shaper", "[atlas]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());
    const auto spec = fonts.resolve("Consolas", false, false, 18);
    REQUIRE(spec.has_value());

    ShapePool pool(1);
    const auto faceId = pool.registerFace(*spec);
    REQUIRE(faceId.has_value());
    const auto metrics = pool.metrics(*faceId);
    REQUIRE(metrics.has_value());

    GlyphAtlas atlas(metrics->cellWidth, metrics->lineHeight);
    const krait::render::RasterFn raster = [&pool](std::uint32_t face, std::uint32_t glyph,
                                                   GlyphBitmap& out) {
        return pool.rasterize(face, glyph, out);
    };

    // Glyph ids are font-specific, so walk a range and require that a decent
    // number of real glyphs land — that proves the FreeType path end to end
    // without hardcoding any particular font's glyph numbering.
    int placed = 0;
    for (std::uint32_t glyph = 1; glyph < 60; ++glyph) {
        if (atlas.get(GlyphKey{.faceId = *faceId, .glyphId = glyph}, raster) != nullptr) {
            ++placed;
        }
    }
    CHECK(placed > 20);
    CHECK(atlas.dirty());
    CHECK(atlas.dirtyBottom() > atlas.dirtyTop());

    SECTION("a rasterised glyph actually has ink") {
        bool anyInk = false;
        for (const std::uint8_t px : atlas.pixels()) {
            if (px != 0) {
                anyInk = true;
                break;
            }
        }
        CHECK(anyInk);
    }
}
