#include "fuzzy.h"
#include "profile.h"
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace krait::app::session;

namespace {

// A temp file that removes itself, so a failing assertion cannot leave a
// sessions.toml behind for the next run to load and pass against.
class TempFile {
  public:
    explicit TempFile(std::string name)
        : m_path((std::filesystem::temp_directory_path() / std::move(name)).string()) {}

    ~TempFile() { std::filesystem::remove(m_path); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return m_path; }

    void write(std::string_view text) const {
        std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    std::string read() const {
        std::ifstream file(m_path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

  private:
    std::string m_path;
};

constexpr const char* kSample = R"(version = 1

[defaults]
port = 22
user = "kla"

[folders."prod"]
accent = "red"

[[session]]
id = "web-1"
name = "Web 1"
folder = "prod/eu"
backend = "ssh"
host = "10.0.0.1"
tags = ["http", "edge"]

[[session]]
id = "lab"
name = "Lab box"
backend = "ssh"
host = "10.9.9.9"
port = 2222
user = "root"
)";

}  // namespace

TEST_CASE("slugify makes a stable id out of a display name", "[session]") {
    CHECK(slugify("Web 1") == "web-1");
    CHECK(slugify("  prod / EU  ") == "prod-eu");
    CHECK(slugify("db.example.com:22") == "db-example-com-22");
    // A name with nothing ASCII in it still needs an id.
    CHECK(slugify("เซิร์ฟเวอร์") == "session");
    CHECK(slugify("") == "session");
}

TEST_CASE("folderChain is the inheritance order", "[session]") {
    CHECK(folderChain("") == std::vector<std::string>{""});
    CHECK(folderChain("prod") == std::vector<std::string>{"", "prod"});
    CHECK(folderChain("prod/eu/db") ==
          std::vector<std::string>{"", "prod", "prod/eu", "prod/eu/db"});
    // A trailing or doubled slash is a typo in a hand-edited file, not a
    // reason to invent an empty folder level.
    CHECK(folderChain("prod//eu/") == std::vector<std::string>{"", "prod", "prod//eu"});
}

TEST_CASE("defaults and folder tables are inherited, explicit keys win", "[session]") {
    TempFile file("krait-profile-inherit.toml");
    file.write(kSample);

    ProfileStore store;
    REQUIRE(store.load(file.path()));
    REQUIRE(store.profiles().size() == 2);

    const Profile* web = store.find("web-1");
    REQUIRE(web != nullptr);
    CHECK(web->name == "Web 1");
    CHECK(web->backend == BackendKind::Ssh);
    CHECK(web->host == "10.0.0.1");
    CHECK(web->port == 22);       // from [defaults]
    CHECK(web->user == "kla");    // from [defaults]
    CHECK(web->accent == "red");  // from [folders."prod"], reached via prod/eu
    CHECK(web->tags == std::vector<std::string>{"http", "edge"});

    const Profile* lab = store.find("lab");
    REQUIRE(lab != nullptr);
    CHECK(lab->port == 2222);    // explicit beats [defaults]
    CHECK(lab->user == "root");  // explicit beats [defaults]
    CHECK(lab->accent.empty());  // not under prod/, so no safety accent
}

TEST_CASE("save keeps inherited values inherited", "[session]") {
    TempFile file("krait-profile-save.toml");
    file.write(kSample);

    ProfileStore store;
    REQUIRE(store.load(file.path()));
    REQUIRE(store.save());

    const std::string written = file.read();
    // The whole point of tracking explicit keys: web-1 RESOLVED to accent="red"
    // and user="kla", but it never SAID either, so saving must not write them
    // onto it. A save that flattens inheritance turns one folder rule into N
    // copies the user then has to edit by hand.
    CHECK(written.find("[defaults]") != std::string::npos);
    // toml++ emits the sub-table header directly ([folders.prod]), never a
    // bare [folders], so match the value rather than a header spelling.
    CHECK(written.find("accent") != std::string::npos);
    // Exactly once: the folder rule, and nowhere else. Two occurrences would
    // mean web-1 had the resolved value written onto it.
    const std::size_t first = written.find("accent");
    CHECK(written.find("accent", first + 1) == std::string::npos);

    ProfileStore reloaded;
    REQUIRE(reloaded.load(file.path()));
    REQUIRE(reloaded.profiles().size() == 2);
    const Profile* web = reloaded.find("web-1");
    REQUIRE(web != nullptr);
    CHECK(web->accent == "red");
    CHECK(web->user == "kla");
    CHECK(web->port == 22);
    CHECK(web->tags == std::vector<std::string>{"http", "edge"});
    CHECK_FALSE(web->isExplicit("accent"));
    CHECK(reloaded.find("lab")->port == 2222);
}

TEST_CASE("a missing file is a first run, a broken one is an error", "[session]") {
    ProfileStore fresh;
    CHECK(fresh.load((std::filesystem::temp_directory_path() / "krait-no-such.toml").string()));
    CHECK(fresh.profiles().empty());
    CHECK(fresh.error().empty());

    TempFile broken("krait-profile-broken.toml");
    broken.write("[[session]\nname = \"oops\"\n");
    ProfileStore store;
    CHECK_FALSE(store.load(broken.path()));
    CHECK_FALSE(store.error().empty());
    // Degrade, do not abort: a hand-edited file is user input.
    CHECK(store.profiles().empty());
}

TEST_CASE("add assigns and de-duplicates ids", "[session]") {
    ProfileStore store;
    Profile first;
    first.name = "Web 1";
    Profile second;
    second.name = "Web 1";

    CHECK(store.add(first) == "web-1");
    CHECK(store.add(second) == "web-1-2");
    CHECK(store.profiles().size() == 2);
    CHECK(store.remove("web-1"));
    CHECK_FALSE(store.remove("web-1"));
    CHECK(store.profiles().size() == 1);
}

TEST_CASE("bulkSet is all-or-nothing on the field name", "[session]") {
    ProfileStore store;
    Profile a;
    a.name = "a";
    Profile b;
    b.name = "b";
    const std::string idA = store.add(a);
    const std::string idB = store.add(b);

    CHECK(store.bulkSet({idA, idB}, "user", "ops"));
    CHECK(store.find(idA)->user == "ops");
    CHECK(store.find(idB)->user == "ops");

    // A typo must not half-apply. Nothing changes and the caller is told.
    CHECK_FALSE(store.bulkSet({idA, idB}, "usr", "nobody"));
    CHECK(store.find(idA)->user == "ops");
    CHECK(store.find(idB)->user == "ops");
}

TEST_CASE("folders fills in intermediate levels", "[session]") {
    ProfileStore store;
    Profile deep;
    deep.name = "db";
    deep.folder = "prod/eu/db";
    deep.markExplicit("folder");
    store.add(deep);

    // "prod" and "prod/eu" hold no profile of their own; a tree without them
    // has holes in it.
    CHECK(store.folders() == std::vector<std::string>{"prod", "prod/eu", "prod/eu/db"});
}

TEST_CASE("fuzzy ranking puts the obvious match first", "[session][fuzzy]") {
    CHECK(fuzzyScore("", "anything") == 0);
    CHECK(fuzzyScore("zzz", "anything") == -1);
    CHECK(fuzzyScore("abc", "ab") == -1);

    // A prefix beats a word start beats a scattered subsequence.
    const int prefix = fuzzyScore("web", "web-1");
    const int wordStart = fuzzyScore("web", "prod-web");
    const int scattered = fuzzyScore("web", "wonderful-elastic-box");
    CHECK(prefix > wordStart);
    CHECK(wordStart > scattered);
    CHECK(scattered >= 0);

    // Case folding is ASCII-only and must not break a non-ASCII name.
    CHECK(fuzzyScore("WEB", "web-1") == fuzzyScore("web", "web-1"));
    CHECK(fuzzyScore("เซ", "เซิร์ฟเวอร์") >= 0);
}
