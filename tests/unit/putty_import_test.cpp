#include "profile.h"
#include "putty_import.h"
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace krait::app::session;

namespace {

PuttyValues sshSession() {
    return {{"Protocol", "ssh"},
            {"HostName", "10.0.0.1"},
            {"UserName", "someone"},
            {"PortNumber", "2222"}};
}

}  // namespace

TEST_CASE("PuTTY's name escaping is undone", "[session][putty]") {
    // mungestr escapes space, backslash, asterisk, question mark, percent,
    // controls, anything above '~', and a LEADING dot.
    CHECK(decodePuttyName("web-1") == "web-1");
    CHECK(decodePuttyName("prod%20web") == "prod web");
    CHECK(decodePuttyName("%2Ehidden") == ".hidden");
    CHECK(decodePuttyName("a%25b") == "a%b");
    // A trailing stub is not an escape. Keeping the byte beats dropping it:
    // this is data from outside the app, and a silently shortened name is a
    // session the user cannot find again.
    CHECK(decodePuttyName("odd%2") == "odd%2");
    CHECK(decodePuttyName("odd%zz") == "odd%zz");
    CHECK(decodePuttyName("").empty());
}

TEST_CASE("an ssh session maps across with its fields", "[session][putty]") {
    const std::optional<Profile> profile = profileFromPutty("web-1", sshSession());
    REQUIRE(profile.has_value());

    CHECK(profile->name == "web-1");
    CHECK(profile->backend == BackendKind::Ssh);
    CHECK(profile->host == "10.0.0.1");
    CHECK(profile->user == "someone");
    CHECK(profile->port == 2222);
    CHECK(profile->id == "web-1");
    // Everything it actually carried is marked explicit, so a later save writes
    // it rather than leaving it to be re-derived from defaults that may differ.
    CHECK(profile->isExplicit("host"));
    CHECK(profile->isExplicit("port"));
    CHECK_FALSE(profile->isExplicit("accent"));
}

TEST_CASE("a telnet session imports as telnet", "[session][putty]") {
    // T54 gave Krait a telnet backend, so these stop being skipped. The value
    // of this test is the assertion that it does NOT come back as ssh: that is
    // the shape of the bug — a profile that imports cleanly and then fails at
    // connect time complaining about the wrong protocol.
    PuttyValues values = sshSession();
    values.front().second = "telnet";
    const std::optional<Profile> profile = profileFromPutty("switch-1", values);
    REQUIRE(profile.has_value());
    CHECK(profile->backend == BackendKind::Telnet);
    CHECK(profile->isExplicit("backend"));
}

TEST_CASE("protocols we do not have yet are reported, not mangled", "[session][putty]") {
    for (const char* protocol : {"raw", "serial"}) {
        PuttyValues values = sshSession();
        values.front().second = protocol;
        // Importing these AS ssh would produce a profile that fails at connect
        // time with a confusing error. The caller counts them instead, and can
        // name them.
        CHECK_FALSE(profileFromPutty("thing", values).has_value());
    }

    // A session with no Protocol value at all is assumed ssh: very old or
    // hand-made keys omit it, and ssh is both the common case and the safe
    // guess — worst case it fails to connect, rather than opening something.
    const PuttyValues noProtocol = {{"HostName", "10.0.0.2"}};
    REQUIRE(profileFromPutty("old", noProtocol).has_value());
}

TEST_CASE("a slash in the name becomes a folder", "[session][putty]") {
    // PuTTY has no folders, so people fake them in the name.
    const std::optional<Profile> nested = profileFromPutty("prod%2Feu%2Fweb-1", sshSession());
    REQUIRE(nested.has_value());
    CHECK(nested->folder == "prod/eu");
    CHECK(nested->name == "web-1");
    CHECK(nested->isExplicit("folder"));

    // " - " is NOT treated as structure: a session honestly named "db - replica"
    // must not become a folder, because a wrong folder is harder to notice than
    // a flat list.
    const std::optional<Profile> dashed = profileFromPutty("db%20-%20replica", sshSession());
    REQUIRE(dashed.has_value());
    CHECK(dashed->folder.empty());
    CHECK(dashed->name == "db - replica");

    // A trailing slash would leave an empty leaf name, which is worse than no
    // folder at all.
    const std::optional<Profile> trailing = profileFromPutty("prod%2F", sshSession());
    REQUIRE(trailing.has_value());
    CHECK(trailing->folder.empty());
    CHECK(trailing->name == "prod/");
}

TEST_CASE("a .ppk path is kept but does not force key auth", "[session][putty]") {
    PuttyValues values = sshSession();
    values.emplace_back("PublicKeyFile", R"(C:\keys\id.ppk)");

    const std::optional<Profile> profile = profileFromPutty("keyed", values);
    REQUIRE(profile.has_value());
    CHECK(profile->keyPath == R"(C:\keys\id.ppk)");
    // libssh cannot read PuTTY's own .ppk format. Keeping the path preserves
    // which key the session used; leaving auth on Auto means the agent still
    // gets its turn first and an unreadable key is not a hard failure.
    CHECK(profile->auth == SshAuth::Auto);
}

TEST_CASE("a nonsense port is ignored rather than used", "[session][putty]") {
    for (const char* port : {"0", "99999", "-1", "", "notanumber"}) {
        PuttyValues values = sshSession();
        values.back().second = port;
        const std::optional<Profile> profile = profileFromPutty("p", values);
        REQUIRE(profile.has_value());
        CHECK(profile->port == 22);  // the default, not the garbage
    }
}

TEST_CASE("imported sessions round-trip through the store", "[session][putty]") {
    ProfileStore store;
    const std::optional<Profile> web = profileFromPutty("prod%2Fweb-1", sshSession());
    REQUIRE(web.has_value());
    const std::string id = store.add(*web);

    const Profile* stored = store.find(id);
    REQUIRE(stored != nullptr);
    CHECK(stored->host == "10.0.0.1");
    CHECK(stored->port == 2222);
    CHECK(stored->folder == "prod");
    CHECK(store.folders() == std::vector<std::string>{"prod"});
}
