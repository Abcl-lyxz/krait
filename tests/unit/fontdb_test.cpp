#include "core/grid/grid.h"
#include "render/shaper/fontdb.h"
#include "render/shaper/run_splitter.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

using krait::core::vt::Grid;
using krait::render::FaceSpec;
using krait::render::FontDb;
using krait::render::Run;
using krait::render::ShapedRun;
using krait::render::ShapePool;
using krait::render::shapeWithFallback;
using krait::render::splitRow;

namespace {

constexpr std::u32string_view kSawasdee = U"สวัสดี";

// Consolas has no Thai, which is what makes it the primary font in every
// fallback test here: the run is guaranteed to come back with .notdef glyphs.
constexpr std::string_view kLatinOnlyFamily = "Consolas";

std::vector<Run> runsFor(std::u32string_view text, int cols = 40) {
    Grid grid(1, cols);
    for (const char32_t cp : text) {
        grid.putChar(cp);
    }
    std::vector<Run> runs;
    splitRow(grid.lineAt(0).cells, grid.clusters(), 0, runs);
    return runs;
}

}  // namespace

TEST_CASE("the font database resolves an installed family to a real file", "[fontdb]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());

    const auto spec = fonts.resolve(kLatinOnlyFamily, false, false, 18);
    REQUIRE(spec.has_value());
    CHECK(spec->pxHeight == 18);
    CHECK(spec->index >= 0);
    // A path FreeType can actually open is the whole point of going through
    // DirectWrite — a family name would be useless to FT_New_Face.
    CHECK_FALSE(spec->path.empty());

    SECTION("bold resolves to a face too") {
        const auto bold = fonts.resolve(kLatinOnlyFamily, true, false, 18);
        REQUIRE(bold.has_value());
        CHECK_FALSE(bold->path.empty());
    }

    SECTION("an absent family is a clean nullopt, not a wrong face") {
        CHECK_FALSE(fonts.resolve("No Such Family At All", false, false, 18).has_value());
    }
}

TEST_CASE("the font database finds a fallback for text the base font lacks", "[fontdb]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());

    const auto spec = fonts.fallbackFor(kSawasdee, kLatinOnlyFamily, false, false, 18);
    REQUIRE(spec.has_value());
    CHECK_FALSE(spec->path.empty());
    CHECK(spec->pxHeight > 0);

    SECTION("the fallback is a different file from the base font") {
        const auto base = fonts.resolve(kLatinOnlyFamily, false, false, 18);
        REQUIRE(base.has_value());
        CHECK(spec->path != base->path);
    }
}

TEST_CASE("firstInstalled picks a font that exists", "[fontdb]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());

    // Deliberately leads with something absent: the point is that it skips it.
    constexpr std::array<std::string_view, 3> candidates{"Definitely Not Installed Mono",
                                                         "Cascadia Mono", "Consolas"};
    const auto chosen = fonts.firstInstalled(candidates);
    REQUIRE(chosen.has_value());
    CHECK(*chosen != "Definitely Not Installed Mono");
    CHECK(fonts.resolve(*chosen, false, false, 16).has_value());
}

// T24's headline behaviour, and the plan's stated verify for this task.
TEST_CASE("a missing-glyph run re-shapes into the fallback face", "[fontdb]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());

    ShapePool pool(2);
    const auto primary = fonts.resolve(kLatinOnlyFamily, false, false, 18);
    REQUIRE(primary.has_value());
    const auto primaryId = pool.registerFace(*primary);
    REQUIRE(primaryId.has_value());

    const std::vector<Run> runs = runsFor(kSawasdee);
    REQUIRE(runs.size() == 1);

    // First establish the precondition rather than assuming it: Consolas really
    // does fail this run, so the fallback below is doing the work.
    std::vector<ShapedRun> direct;
    REQUIRE(pool.shapeAll(runs, *primaryId, false, direct, std::chrono::seconds{5}));
    REQUIRE(direct[0].missingGlyphs);

    std::vector<ShapedRun> shaped;
    const std::vector<std::uint32_t> faces =
        shapeWithFallback(pool, fonts, runs, *primaryId, kLatinOnlyFamily, 18, false, shaped);

    REQUIRE(shaped.size() == 1);
    REQUIRE(faces.size() == 1);
    CHECK(faces[0] != *primaryId);         // a different face answered
    CHECK_FALSE(shaped[0].missingGlyphs);  // and it covers the text
    CHECK(shaped[0].faceId == faces[0]);
    REQUIRE(shaped[0].glyphs.size() >= 4);

    // The cluster mapping has to survive the re-shape, or the renderer would
    // place fallback glyphs in the wrong cells.
    for (const auto& glyph : shaped[0].glyphs) {
        CHECK(glyph.cluster < runs[0].clusters.size());
    }
}

TEST_CASE("a run the primary font covers is never sent to a fallback", "[fontdb]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());

    ShapePool pool(1);
    const auto primary = fonts.resolve(kLatinOnlyFamily, false, false, 18);
    REQUIRE(primary.has_value());
    const auto primaryId = pool.registerFace(*primary);
    REQUIRE(primaryId.has_value());

    const std::vector<Run> runs = runsFor(U"plain ascii");
    std::vector<ShapedRun> shaped;
    const std::vector<std::uint32_t> faces =
        shapeWithFallback(pool, fonts, runs, *primaryId, kLatinOnlyFamily, 18, false, shaped);

    REQUIRE(faces.size() == runs.size());
    CHECK(std::ranges::all_of(faces, [&](std::uint32_t id) { return id == *primaryId; }));
    CHECK_FALSE(shaped[0].missingGlyphs);
}

TEST_CASE("a mixed Latin and Thai row falls back per run, not per row", "[fontdb]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());

    ShapePool pool(2);
    const auto primary = fonts.resolve(kLatinOnlyFamily, false, false, 18);
    REQUIRE(primary.has_value());
    const auto primaryId = pool.registerFace(*primary);
    REQUIRE(primaryId.has_value());

    // The realistic case: a prompt followed by Thai.
    std::u32string line = U"user@host:~$ ";
    line += kSawasdee;
    const std::vector<Run> runs = runsFor(line, 60);
    REQUIRE(runs.size() == 2);  // the script split from T23

    std::vector<ShapedRun> shaped;
    const std::vector<std::uint32_t> faces =
        shapeWithFallback(pool, fonts, runs, *primaryId, kLatinOnlyFamily, 18, false, shaped);

    REQUIRE(faces.size() == 2);
    // The Latin half keeps the chosen terminal font — a whole-row fallback would
    // silently replace the user's font for the entire line.
    CHECK(faces[0] == *primaryId);
    CHECK(faces[1] != *primaryId);
    CHECK_FALSE(shaped[0].missingGlyphs);
    CHECK_FALSE(shaped[1].missingGlyphs);
}

TEST_CASE("the ligature toggle changes what a ligature font produces", "[fontdb]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());

    // Cascadia CODE has programming ligatures; Cascadia MONO is the same design
    // with them removed, so it must never be substituted here.
    constexpr std::array<std::string_view, 2> ligatureFonts{"Cascadia Code", "Fira Code"};
    const auto family = fonts.firstInstalled(ligatureFonts);
    if (!family.has_value()) {
        SUCCEED("no ligature font installed; the toggle is covered by the cache-key test");
        return;
    }

    ShapePool pool(1);
    const auto spec = fonts.resolve(*family, false, false, 20);
    REQUIRE(spec.has_value());
    const auto faceId = pool.registerFace(*spec);
    REQUIRE(faceId.has_value());

    // "!=" is the canonical programming ligature: two cells, and with calt on
    // the font replaces the pair with one arrow glyph.
    const std::vector<Run> runs = runsFor(U"!=");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].clusters.size() == 2);

    std::vector<ShapedRun> off;
    std::vector<ShapedRun> on;
    REQUIRE(pool.shapeAll(runs, *faceId, false, off, std::chrono::seconds{5}));
    REQUIRE(pool.shapeAll(runs, *faceId, true, on, std::chrono::seconds{5}));

    // Ligatures off must leave both characters as their own glyphs. On, the font
    // is free to merge them — so assert the two differ rather than assuming a
    // particular glyph count, which is the font's business, not ours.
    CHECK(off[0].glyphs.size() == 2);
    const bool merged = on[0].glyphs.size() < off[0].glyphs.size();
    const bool substituted = on[0].glyphs.size() == off[0].glyphs.size() &&
                             on[0].glyphs[0].glyphId != off[0].glyphs[0].glyphId;
    CHECK((merged || substituted));
}

TEST_CASE("registering the same face twice reuses one id", "[fontdb]") {
    const FontDb fonts;
    REQUIRE(fonts.valid());

    ShapePool pool(1);
    const auto spec = fonts.resolve(kLatinOnlyFamily, false, false, 18);
    REQUIRE(spec.has_value());

    const auto first = pool.registerFace(*spec);
    const auto second = pool.registerFace(*spec);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    // shapeWithFallback registers a fallback face for every missing-glyph run it
    // sees, on every frame. Without dedupe that grows the spec table without
    // bound for the life of the session.
    CHECK(*first == *second);
}
