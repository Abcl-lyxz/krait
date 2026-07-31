#include "core/grid/grid.h"
#include "render/shaper/run_splitter.h"
#include "render/shaper/shape_pool.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using krait::core::vt::Grid;
using krait::render::FaceSpec;
using krait::render::Run;
using krait::render::ShapedRun;
using krait::render::ShapePool;
using krait::render::splitRow;

namespace {

// สวัสดี — "hello". Six codepoints, FOUR grapheme clusters, because U+0E31 and
// U+0E35 are nonspacing marks that ride their base consonant. That 6-into-4 is
// exactly what the shaper must preserve: it must not re-segment, and it must map
// every output glyph back to one of the four cells.
constexpr std::u32string_view kSawasdee = U"\u0E2A\u0E27\u0E31\u0E2A\u0E14\u0E35";

// Windows ships all of these. Tahoma and Leelawadee UI cover Thai; Consolas
// does NOT, which is what makes it a usable "missing glyph" probe (and the
// precondition for T24's fallback chain).
std::string findFont(std::initializer_list<const char*> candidates) {
    for (const char* candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::string thaiFont() {
    return findFont({"C:/Windows/Fonts/tahoma.ttf", "C:/Windows/Fonts/leelawui.ttf",
                     "C:/Windows/Fonts/LeelaUIb.ttf"});
}

std::string latinOnlyFont() {
    return findFont({"C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf"});
}

// Feeds text through the real grid so the clusters under test are the ones T20
// actually stores, not a hand-built approximation.
std::vector<Run> rowRuns(Grid& grid, int row = 0) {
    std::vector<Run> runs;
    splitRow(grid.lineAt(row).cells, grid.clusters(), row, runs);
    return runs;
}

void write(Grid& grid, std::u32string_view text) {
    for (const char32_t cp : text) {
        grid.putChar(cp);
    }
}

}  // namespace

TEST_CASE("run splitter keeps Thai as one run of four clusters", "[shaper]") {
    Grid grid(1, 20);
    write(grid, kSawasdee);

    const std::vector<Run> runs = rowRuns(grid);
    REQUIRE(runs.size() == 1);
    // Six codepoints in, four clusters out: the marks did not claim cells.
    CHECK(runs[0].text.size() == 6);
    REQUIRE(runs[0].clusters.size() == 4);
    CHECK(runs[0].col == 0);
    CHECK_FALSE(runs[0].rightToLeft);
    for (std::size_t i = 0; i < runs[0].clusters.size(); ++i) {
        CHECK(runs[0].clusters[i].cells == 1);
        CHECK(runs[0].clusters[i].col == static_cast<int>(i));
    }
    CHECK(runs[0].clusters[0].len == 1);  // ส
    CHECK(runs[0].clusters[1].len == 2);  // ว + ั
    CHECK(runs[0].clusters[2].len == 1);  // ส
    CHECK(runs[0].clusters[3].len == 2);  // ด + ี
}

TEST_CASE("run splitter breaks on a script change", "[shaper]") {
    Grid grid(1, 40);
    write(grid, U"ab");
    write(grid, kSawasdee);
    write(grid, U"cd");

    const std::vector<Run> runs = rowRuns(grid);
    // Without this split HarfBuzz would guess the script from 'a' and shape the
    // Thai as Latin, silently dropping every mark's positioning.
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].text == U"ab");
    CHECK(runs[1].clusters.size() == 4);
    CHECK(runs[2].text == U"cd");
    CHECK(runs[0].script != runs[1].script);
}

TEST_CASE("run splitter treats digits and spaces as weak, gaps as breaks", "[shaper]") {
    SECTION("a printed space stays inside the run") {
        Grid grid(1, 20);
        write(grid, U"a b");
        const std::vector<Run> runs = rowRuns(grid);
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].text == U"a b");
    }
    SECTION("an unwritten cell ends the run") {
        Grid grid(1, 20);
        write(grid, U"ab");
        grid.cursorSet(0, 5);
        write(grid, U"cd");
        const std::vector<Run> runs = rowRuns(grid);
        REQUIRE(runs.size() == 2);
        CHECK(runs[0].text == U"ab");
        CHECK(runs[1].text == U"cd");
        CHECK(runs[1].col == 5);
    }
    SECTION("leading digits adopt the following strong script") {
        Grid grid(1, 20);
        write(grid, U"12");
        write(grid, kSawasdee);
        const std::vector<Run> runs = rowRuns(grid);
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].script == krait::render::scriptOf(U'\u0E2A'));
    }
}

TEST_CASE("run splitter splits on bold and never on colour", "[shaper]") {
    SECTION("bold starts a new run") {
        Grid grid(1, 20);
        write(grid, U"ab");
        grid.pen.flags |= krait::core::vt::Attr::kBold;
        write(grid, U"cd");
        const std::vector<Run> runs = rowRuns(grid);
        REQUIRE(runs.size() == 2);
        CHECK(runs[0].shaping == 0);
        CHECK(runs[1].shaping == krait::core::vt::Attr::kBold);
    }
    SECTION("a colour change does not") {
        Grid grid(1, 20);
        write(grid, U"ab");
        grid.pen.fg = krait::core::vt::Color::indexed(9);
        write(grid, U"cd");
        // Colour is per-cell draw state, not a shaping input: splitting here
        // would miss the cache on every recolour of identical text.
        const std::vector<Run> runs = rowRuns(grid);
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].text == U"abcd");
    }
}

TEST_CASE("run splitter counts a wide cluster's second cell without re-measuring", "[shaper]") {
    Grid grid(1, 20);
    write(grid, U"\u4E2D\u6587");  // 中文, two cells each

    const std::vector<Run> runs = rowRuns(grid);
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].clusters.size() == 2);
    CHECK(runs[0].clusters[0].cells == 2);
    CHECK(runs[0].clusters[0].col == 0);
    CHECK(runs[0].clusters[1].cells == 2);
    CHECK(runs[0].clusters[1].col == 2);  // the trailing half was skipped, not shaped
}

TEST_CASE("Thai shapes to positioned marks over four clusters", "[shaper]") {
    const std::string font = thaiFont();
    REQUIRE_FALSE(font.empty());  // every Windows install ships Tahoma

    ShapePool pool(2);
    const auto faceId = pool.registerFace(FaceSpec{.path = font, .index = 0, .pxHeight = 18});
    REQUIRE(faceId.has_value());

    Grid grid(1, 20);
    write(grid, kSawasdee);
    const std::vector<Run> runs = rowRuns(grid);
    REQUIRE(runs.size() == 1);

    std::vector<ShapedRun> shaped;
    // Explicit timeout, matching fontdb_test and the run below: shapeAll's
    // 8 ms default is a sub-frame RENDER budget, and returning false when it
    // expires is the documented graceful path, not a failure. Asserting that
    // budget inside a correctness test makes the test flaky on a loaded CI
    // runner — which is exactly how it started failing once M2 made the suite
    // heavier. What this case is about is Thai mark positioning.
    REQUIRE(pool.shapeAll(runs, *faceId, false, shaped, std::chrono::seconds{10}));
    REQUIRE(shaped.size() == 1);

    const ShapedRun& out = shaped[0];
    CHECK_FALSE(out.missingGlyphs);  // Tahoma covers Thai
    REQUIRE(out.glyphs.size() >= 4);

    // Every glyph maps back to one of the four clusters, and all four are
    // covered — this is the property the renderer needs to place glyphs in
    // cells, and it is what "shapes to clusters" means.
    std::vector<bool> covered(runs[0].clusters.size(), false);
    for (const auto& glyph : out.glyphs) {
        REQUIRE(glyph.cluster < runs[0].clusters.size());
        covered[glyph.cluster] = true;
    }
    CHECK(std::ranges::all_of(covered, [](bool c) { return c; }));

    // A nonspacing mark must not advance the pen. If Thai were shaped as plain
    // Latin — the failure mode the script split exists to prevent — every glyph
    // would carry a positive advance instead.
    CHECK(std::ranges::any_of(out.glyphs, [](const auto& g) { return g.xAdvance == 0; }));
}

TEST_CASE("a face without coverage reports missing glyphs", "[shaper]") {
    const std::string font = latinOnlyFont();
    REQUIRE_FALSE(font.empty());

    ShapePool pool(1);
    const auto faceId = pool.registerFace(FaceSpec{.path = font, .index = 0, .pxHeight = 18});
    REQUIRE(faceId.has_value());

    Grid grid(1, 20);
    write(grid, kSawasdee);
    const std::vector<Run> runs = rowRuns(grid);

    std::vector<ShapedRun> shaped;
    REQUIRE(pool.shapeAll(runs, *faceId, false, shaped, std::chrono::seconds{10}));
    // Consolas has no Thai. This flag is what T24's fallback chain triggers on.
    CHECK(shaped[0].missingGlyphs);
}

TEST_CASE("shaped runs are cached, and the cache is bounded", "[shaper]") {
    const std::string font = latinOnlyFont();
    REQUIRE_FALSE(font.empty());

    ShapePool pool(2);
    const auto faceId = pool.registerFace(FaceSpec{.path = font, .index = 0, .pxHeight = 16});
    REQUIRE(faceId.has_value());

    Grid grid(1, 40);
    write(grid, U"cache me");
    const std::vector<Run> runs = rowRuns(grid);
    std::vector<ShapedRun> first;
    std::vector<ShapedRun> second;

    REQUIRE(pool.shapeAll(runs, *faceId, false, first, std::chrono::seconds{10}));
    CHECK(pool.cacheMisses() == 1);
    CHECK(pool.cacheHits() == 0);

    REQUIRE(pool.shapeAll(runs, *faceId, false, second, std::chrono::seconds{10}));
    CHECK(pool.cacheMisses() == 1);  // unchanged: served from the cache
    CHECK(pool.cacheHits() == 1);
    REQUIRE(first[0].glyphs.size() == second[0].glyphs.size());
    CHECK(first[0].glyphs[0].glyphId == second[0].glyphs[0].glyphId);

    SECTION("the ligature flag is part of the key") {
        std::vector<ShapedRun> third;
        REQUIRE(pool.shapeAll(runs, *faceId, true, third, std::chrono::seconds{10}));
        CHECK(pool.cacheMisses() == 2);  // same text, different key
        CHECK(pool.cacheSize() == 2);
    }
}

TEST_CASE("the shaped-run cache evicts instead of growing", "[shaper]") {
    using krait::render::ShapeCache;

    // Distinct key per iteration: a codepoint sequence derived from i.
    const auto keyFor = [](std::size_t i) {
        std::u32string text = U"run";
        for (std::size_t n = i; n > 0; n /= 10) {
            text.push_back(static_cast<char32_t>(U'0' + (n % 10)));
        }
        return ShapeCache::Key{.faceId = 0, .shaping = 0, .ligatures = false, .text = text};
    };

    // A realistic entry: an 80-column run carries roughly that many glyphs.
    const ShapedRun payload{.glyphs = std::vector<krait::render::ShapedGlyph>(80), .faceId = 0};

    ShapeCache cache;
    const ShapeCache::Key first = keyFor(1);
    std::size_t inserted = 0;
    while (cache.bytes() + ShapeCache::costOf(keyFor(inserted + 1), payload) <=
           ShapeCache::kMaxCacheBytes) {
        ++inserted;
        cache.insert(keyFor(inserted), payload);
    }
    REQUIRE(inserted > 100);  // the budget holds a useful number of runs
    CHECK(cache.bytes() <= ShapeCache::kMaxCacheBytes);
    // Deliberately NOT find(first) here: find() touches, and touching the entry
    // the sections below expect to be evicted first would move it to the front
    // of the LRU and make them pass or fail for the wrong reason.
    CHECK(cache.size() == inserted);

    SECTION("crossing the budget evicts the oldest, not an arbitrary entry") {
        for (std::size_t i = inserted + 1; i <= inserted + 200; ++i) {
            cache.insert(keyFor(i), payload);
        }
        CHECK(cache.bytes() <= ShapeCache::kMaxCacheBytes);
        CHECK(cache.find(first) == nullptr);
        CHECK(cache.find(keyFor(inserted + 200)) != nullptr);
    }

    SECTION("a touched entry survives the next round of evictions") {
        const ShapeCache::Key survivor = keyFor(2);
        REQUIRE(cache.find(survivor) != nullptr);  // touching moves it to the front
        for (std::size_t i = inserted + 1; i <= inserted + 200; ++i) {
            cache.insert(keyFor(i), payload);
        }
        CHECK(cache.find(survivor) != nullptr);
        CHECK(cache.find(first) == nullptr);  // untouched, and older
    }

    SECTION("one entry may not own the whole budget") {
        const ShapeCache::Key huge{.faceId = 0, .shaping = 0, .ligatures = false, .text = U"huge"};
        const ShapedRun enormous{
            .glyphs = std::vector<krait::render::ShapedGlyph>(ShapeCache::kMaxCacheBytes /
                                                              sizeof(krait::render::ShapedGlyph)),
            .faceId = 0};
        cache.insert(huge, enormous);
        CHECK(cache.find(huge) == nullptr);
        CHECK(cache.bytes() <= ShapeCache::kMaxCacheBytes);
    }

    SECTION("a wide Thai row is cacheable — a codepoint cap would have excluded it") {
        // 200 columns of สวัสดี-density text is 300 codepoints, which a
        // 256-codepoint cap rejected outright, re-shaping it on every frame.
        std::u32string wide;
        for (int i = 0; i < 50; ++i) {
            wide += kSawasdee;
        }
        const ShapeCache::Key thai{.faceId = 0, .shaping = 0, .ligatures = false, .text = wide};
        cache.insert(thai, payload);
        CHECK(cache.find(thai) != nullptr);
    }
}

TEST_CASE("run splitter survives cells with nothing shapeable", "[shaper]") {
    using krait::core::vt::Cell;
    krait::core::vt::ClusterPool pool;

    SECTION("a cluster ref with no pool entry breaks the run instead of shaping the tag") {
        // ClusterPool::lookup documents "a ref that outlived its pool" as a real
        // state. The tag bits are not a codepoint and must never reach HarfBuzz.
        std::vector<Cell> cells(4);
        cells[0].ch = U'a';
        cells[1].ch = krait::core::vt::kClusterTag | 999;  // never interned
        cells[2].ch = U'b';
        std::vector<Run> runs;
        splitRow(cells, pool, 0, runs);
        REQUIRE(runs.size() == 2);
        CHECK(runs[0].text == U"a");
        CHECK(runs[1].text == U"b");
        CHECK(runs[1].col == 2);
    }

    SECTION("a leading wide-trailing half is skipped, not shaped") {
        // Reachable after a reflow that leaves a wide pair's left half on the
        // previous row.
        std::vector<Cell> cells(3);
        cells[0].ch = krait::core::vt::kWideTrailing;
        cells[1].ch = U'x';
        std::vector<Run> runs;
        splitRow(cells, pool, 0, runs);
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].text == U"x");
        CHECK(runs[0].col == 1);
    }
}

TEST_CASE("face metrics give a cell advance, not the widest glyph", "[shaper]") {
    const std::string font = latinOnlyFont();
    REQUIRE_FALSE(font.empty());

    ShapePool pool(1);
    const auto faceId = pool.registerFace(FaceSpec{.path = font, .index = 0, .pxHeight = 20});
    REQUIRE(faceId.has_value());

    const auto metrics = pool.metrics(*faceId);
    REQUIRE(metrics.has_value());
    CHECK(metrics->cellWidth > 0);
    CHECK(metrics->ascent > 0);
    CHECK(metrics->descent >= 0);
    CHECK(metrics->lineHeight > 0);
    // A monospace cell is narrower than it is tall in every terminal font.
    CHECK(metrics->cellWidth < metrics->lineHeight);
}

TEST_CASE("registering a face that does not exist fails at registration", "[shaper]") {
    ShapePool pool(1);
    // Not a silent blank-glyph frame later: the caller learns here.
    CHECK_FALSE(pool.registerFace(FaceSpec{.path = "C:/nope/not-a-font.ttf"}).has_value());
}

// Hidden by default ([.]); run with `krait-core-tests "[bench]"`. Records the
// cold/warm shaping cost that bench/baselines/t23-shaper.json pins.
TEST_CASE("shaping throughput", "[.][bench]") {
    const std::string font = latinOnlyFont();
    REQUIRE_FALSE(font.empty());

    ShapePool pool(0);
    const auto faceId = pool.registerFace(FaceSpec{.path = font, .index = 0, .pxHeight = 18});
    REQUIRE(faceId.has_value());

    // 63 distinct rows of 78 columns: one screenful where nothing is cached.
    constexpr int kRows = 63;
    std::vector<Run> runs;
    for (int row = 0; row < kRows; ++row) {
        Grid grid(1, 80);
        for (int col = 0; col < 78; ++col) {
            grid.putChar(static_cast<char32_t>(0x21 + ((row * 78 + col) % 94)));
        }
        splitRow(grid.lineAt(0).cells, grid.clusters(), row, runs);
    }

    std::vector<ShapedRun> out;
    const auto coldStart = std::chrono::steady_clock::now();
    REQUIRE(pool.shapeAll(runs, *faceId, false, out, std::chrono::seconds{10}));
    const auto coldMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - coldStart)
            .count();

    const auto warmStart = std::chrono::steady_clock::now();
    REQUIRE(pool.shapeAll(runs, *faceId, false, out));
    const auto warmMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warmStart)
            .count();

    std::printf("[bench] runs=%zu workers=%u cold=%.3f ms warm=%.3f ms hits=%llu misses=%llu\n",
                runs.size(), pool.workerCount(), coldMs, warmMs,
                static_cast<unsigned long long>(pool.cacheHits()),
                static_cast<unsigned long long>(pool.cacheMisses()));
    CHECK(warmMs < coldMs);  // the cache has to be worth its memory
}
