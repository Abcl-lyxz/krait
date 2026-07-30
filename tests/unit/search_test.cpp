#include "core/grid/search.h"
#include "core/terminal/session.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

using namespace krait::core::vt;

namespace {

void feed(Session& session, std::string_view text) {
    session.feed({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::string selected(std::string_view text, std::size_t at) {
    const SmartSelection sel = smartSelect(text, at);
    return std::string(text.substr(sel.begin, sel.end - sel.begin));
}

}  // namespace

TEST_CASE("a row becomes text without its padding", "[core][search]") {
    Session session(4, 20);
    feed(session, "hello");

    const std::string text =
        lineText(session.grid().viewportRows().front(), session.grid().clusters());
    // A grid row is always `cols` wide. Searching a screenful of 80-column rows
    // would otherwise be mostly searching spaces.
    CHECK(text == "hello");

    // A wide cluster contributes ONE character, not two — its trailing cell is
    // the same character, not a second one.
    Session cjk(4, 20);
    feed(cjk, "\xe6\x97\xa5\xe6\x9c\xac");  // 日本
    const std::string wide = lineText(cjk.grid().viewportRows().front(), cjk.grid().clusters());
    CHECK(wide == "\xe6\x97\xa5\xe6\x9c\xac");
}

TEST_CASE("literal search finds every occurrence, case-insensitively", "[core][search]") {
    Session session(6, 40);
    feed(session, "Error: disk full\r\nerror again\r\nall good\r\n");

    const auto hits = searchScrollback(session.grid(), "error", {});
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 2);
    CHECK((*hits)[0].begin == 0);
    CHECK((*hits)[0].end == 5);

    const auto sensitive =
        searchScrollback(session.grid(), "error", {.regex = false, .caseSensitive = true});
    REQUIRE(sensitive.has_value());
    CHECK(sensitive->size() == 1);

    CHECK(searchScrollback(session.grid(), "", {})->empty());
    CHECK(searchScrollback(session.grid(), "nowhere", {})->empty());
}

TEST_CASE("a bad regex is an answer, not a crash", "[core][search]") {
    Session session(4, 20);
    feed(session, "abc\r\n");

    // A search box is user input, and a half-typed pattern is the NORMAL state
    // while someone types. std::regex throws; nothing may escape src/core.
    const auto bad = searchScrollback(session.grid(), "[unclosed", {.regex = true});
    REQUIRE_FALSE(bad.has_value());
    CHECK_FALSE(bad.error().empty());

    const auto good = searchScrollback(session.grid(), "a.c", {.regex = true});
    REQUIRE(good.has_value());
    CHECK(good->size() == 1);
}

TEST_CASE("search results are bounded", "[core][search]") {
    Session session(6, 40);
    for (int i = 0; i < 200; ++i) {
        feed(session, "match\r\n");
    }

    // A pattern that matches everywhere is the easy case to get wrong: a search
    // returning every hit is a search that hangs the UI rendering its own
    // results.
    const auto hits = searchScrollback(session.grid(), "match",
                                       {.regex = false, .caseSensitive = false, .maxHits = 10});
    REQUIRE(hits.has_value());
    CHECK(hits->size() == 10);

    // A zero-width regex match must not fill the cap with nothing.
    const auto empty = searchScrollback(session.grid(), "x*",
                                        {.regex = true, .caseSensitive = false, .maxHits = 10});
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("double-click takes the whole URL", "[core][search]") {
    const std::string_view line = "see https://example.test/a/b?q=1 for details";
    // The classic terminal annoyance is getting back only the hostname. The
    // thing on screen is one thing.
    CHECK(selected(line, 12) == "https://example.test/a/b?q=1");
    CHECK(selected(line, 4) == "https://example.test/a/b?q=1");

    // Trailing punctuation belongs to the sentence, not the link — a URL at the
    // end of a log line almost always has a period after it.
    CHECK(selected("go to https://example.test/x.", 10) == "https://example.test/x");
    CHECK(selected("(https://example.test)", 5) == "https://example.test");
}

TEST_CASE("paths stay whole and words stay words", "[core][search]") {
    // A path is one thing, in both spellings.
    CHECK(selected("open /usr/local/bin/krait now", 8) == "/usr/local/bin/krait");
    CHECK(selected(R"(at C:\Users\Kla\krait.toml today)", 8) == R"(C:\Users\Kla\krait.toml)");
    CHECK(selected("run ./scripts/build.sh", 8) == "./scripts/build.sh");

    // In prose a dot ends the word — which is why the path rule has to run
    // first, and why `./a.txt` behaves differently from `example.test`.
    CHECK(selected("visit example.test today", 8) == "example");
    CHECK(selected("some-package_name here", 4) == "some-package_name");

    // Sitting on punctuation takes that byte rather than guessing a neighbour.
    CHECK(selected("a , b", 2) == ",");
    CHECK(selected("", 0).empty());
    // Past the end clamps rather than reading off it.
    CHECK(selected("abc", 99) == "abc");
}
