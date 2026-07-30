#include "actions.h"
#include "palette.h"
#include "profile.h"
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace krait::app::session;

namespace {

Profile makeProfile(std::string name, std::string folder, std::string host) {
    Profile profile;
    profile.name = std::move(name);
    profile.folder = std::move(folder);
    profile.host = std::move(host);
    profile.backend = BackendKind::Ssh;
    profile.markExplicit("name");
    profile.markExplicit("folder");
    profile.markExplicit("host");
    return profile;
}

ProfileStore sampleStore() {
    ProfileStore store;
    store.add(makeProfile("web-1", "prod/eu", "10.0.0.1"));
    store.add(makeProfile("web-2", "prod/eu", "10.0.0.2"));
    store.add(makeProfile("db", "prod", "10.0.1.1"));
    store.add(makeProfile("laptop", "", "127.0.0.1"));
    return store;
}

}  // namespace

TEST_CASE("every action is reachable and uniquely identified", "[session][palette]") {
    std::set<std::string_view> ids;
    for (const Action& action : allActions()) {
        CHECK_FALSE(action.id.empty());
        CHECK_FALSE(action.label.empty());
        // A duplicate id means one of two actions is unreachable by keybinding,
        // and which one would depend on lookup order.
        CHECK(ids.insert(action.id).second);
        CHECK(findAction(action.id) != nullptr);
    }
    CHECK(findAction("nope.not.here") == nullptr);

    // rules/ui.md: a feature reachable only by mouse is incomplete work. Every
    // action is at minimum findable in the palette, which is why the label and
    // id checks above are the real gate rather than the shortcut.
    REQUIRE(findAction("palette.open") != nullptr);
    CHECK_FALSE(findAction("palette.open")->shortcut.empty());
}

TEST_CASE("an empty query offers sessions before commands", "[session][palette]") {
    const ProfileStore store = sampleStore();
    const std::vector<PaletteEntry> entries = rankPalette("", store);

    REQUIRE(entries.size() == store.profiles().size() + allActions().size());
    // With nothing typed the useful default is "where do you want to connect",
    // not an alphabetical list of commands.
    CHECK(entries.front().kind == PaletteEntry::Kind::Session);
}

TEST_CASE("a session is found by host, not only by name", "[session][palette]") {
    const ProfileStore store = sampleStore();

    const std::vector<PaletteEntry> byHost = rankPalette("10.0.1.1", store);
    REQUIRE_FALSE(byHost.empty());
    CHECK(byHost.front().kind == PaletteEntry::Kind::Session);
    CHECK(byHost.front().label == "db");

    // People remember the machine, not what they called the profile three
    // months ago — matching only the name is how a palette stops being used.
    const std::vector<PaletteEntry> byFolder = rankPalette("prod/eu", store);
    REQUIRE_FALSE(byFolder.empty());
    CHECK(byFolder.front().kind == PaletteEntry::Kind::Session);
}

TEST_CASE("an action is found by the word people actually type", "[session][palette]") {
    const ProfileStore store;

    // "quit" appears nowhere in "Close session" — it is in the keywords, which
    // is the whole reason keywords exist.
    const std::vector<PaletteEntry> quit = rankPalette("quit", store);
    REQUIRE_FALSE(quit.empty());
    CHECK(quit.front().id == "session.close");

    const std::vector<PaletteEntry> find = rankPalette("grep", store);
    REQUIRE_FALSE(find.empty());
    CHECK(find.front().id == "edit.search");
}

TEST_CASE("non-matches are dropped and ordering is stable", "[session][palette]") {
    const ProfileStore store = sampleStore();
    CHECK(rankPalette("zzzzqqq", store).empty());

    // The same query twice gives the same order. A list that reshuffles under
    // an unchanged prefix is a list you cannot aim at.
    const std::vector<PaletteEntry> a = rankPalette("we", store);
    const std::vector<PaletteEntry> b = rankPalette("we", store);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].id == b[i].id);
    }
}

TEST_CASE("the tree is derived, with no holes in it", "[session][palette]") {
    const ProfileStore store = sampleStore();
    const std::vector<TreeRow> rows = buildTree(store);

    // Root-level sessions first, then folders depth-first.
    REQUIRE_FALSE(rows.empty());
    CHECK_FALSE(rows.front().isFolder);
    CHECK(rows.front().label == "laptop");

    // "prod" holds one session directly and "prod/eu" holds two; both levels
    // must appear even though nothing declares "prod" as a folder anywhere.
    bool sawProd = false;
    bool sawEu = false;
    for (const TreeRow& row : rows) {
        if (row.isFolder && row.id == "prod") {
            sawProd = true;
            CHECK(row.depth == 0);
            CHECK(row.label == "prod");
        }
        if (row.isFolder && row.id == "prod/eu") {
            sawEu = true;
            CHECK(row.depth == 1);
            // The LAST segment is the label; the full path is the id.
            CHECK(row.label == "eu");
        }
    }
    CHECK(sawProd);
    CHECK(sawEu);

    // Every profile appears exactly once.
    int sessionRows = 0;
    for (const TreeRow& row : rows) {
        sessionRows += row.isFolder ? 0 : 1;
    }
    CHECK(static_cast<std::size_t>(sessionRows) == store.profiles().size());
}

TEST_CASE("the palette finds a session in well under 100 ms", "[session][palette][bench]") {
    // The milestone's acceptance number. 2000 profiles is a lot of real estate
    // for one person, and the palette has to feel instant at that size or the
    // keyboard-first story does not hold.
    ProfileStore store;
    for (int i = 0; i < 2000; ++i) {
        store.add(makeProfile("host-" + std::to_string(i), "dc/rack-" + std::to_string(i % 40),
                              "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256)));
    }

    const auto start = std::chrono::steady_clock::now();
    const std::vector<PaletteEntry> entries = rankPalette("hst1234", store);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    CHECK(elapsed.count() < 100);
    // And it still found something, so the timing is not measuring an early
    // bail-out.
    CHECK_FALSE(entries.empty());
}
