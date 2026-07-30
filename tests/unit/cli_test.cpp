#include "cli.h"
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace krait::app::session;

namespace {

Launch parse(std::vector<std::string> args) {
    args.insert(args.begin(), "krait.exe");
    return parseCommandLine(args);
}

}  // namespace

TEST_CASE("no arguments opens the default session", "[session][cli]") {
    CHECK(parse({}).kind == Launch::Kind::Default);
}

TEST_CASE("a bare word is a profile, never a host", "[session][cli]") {
    const Launch launch = parse({"prod"});
    REQUIRE(launch.kind == Launch::Kind::Profile);
    CHECK(launch.profileName == "prod");

    // The reason this is not an ad-hoc connection: `krait prod` meaning
    // "connect to the machine literally named prod" is how someone ends up on a
    // box they did not mean. The saved profile is what they wanted, and if
    // there is no such profile the caller says so rather than dialling.
    CHECK(parse({"--profile", "prod"}).profileName == "prod");
    CHECK(parse({"--profile"}).error);
}

TEST_CASE("ssh targets split into user, host and port", "[session][cli]") {
    std::string user;
    std::string host;
    int port = 22;

    REQUIRE(parseTarget("example.test", &user, &host, &port));
    CHECK(user.empty());
    CHECK(host == "example.test");
    CHECK(port == 22);

    REQUIRE(parseTarget("someone@example.test:2222", &user, &host, &port));
    CHECK(user == "someone");
    CHECK(host == "example.test");
    CHECK(port == 2222);

    // Bare IPv6 has no port. "a number follows the last colon" is NOT the rule
    // — fe80::1 ends in a perfectly good number, and reading it as a port
    // connects to a different address on port 1 without saying anything.
    port = 22;
    REQUIRE(parseTarget("fe80::1", &user, &host, &port));
    CHECK(host == "fe80::1");
    CHECK(port == 22);

    port = 22;
    REQUIRE(parseTarget("2001:db8::dead:beef", &user, &host, &port));
    CHECK(host == "2001:db8::dead:beef");
    CHECK(port == 22);

    // Brackets are the unambiguous spelling, and the only one where a port may
    // follow a colon-rich host.
    port = 22;
    REQUIRE(parseTarget("[fe80::1]:2222", &user, &host, &port));
    CHECK(host == "fe80::1");
    CHECK(port == 2222);

    port = 22;
    REQUIRE(parseTarget("ops@[fe80::1]", &user, &host, &port));
    CHECK(user == "ops");
    CHECK(host == "fe80::1");
    CHECK(port == 22);

    CHECK_FALSE(parseTarget("", &user, &host, &port));
    CHECK_FALSE(parseTarget("host:0", &user, &host, &port));
    CHECK_FALSE(parseTarget("host:99999", &user, &host, &port));
    // A colon with no usable port is a typo, not a hostname with a colon in it.
    CHECK_FALSE(parseTarget("host:", &user, &host, &port));
    CHECK_FALSE(parseTarget("host:ssh", &user, &host, &port));
    CHECK_FALSE(parseTarget("[fe80::1", &user, &host, &port));
}

TEST_CASE("krait ssh builds an ad-hoc profile", "[session][cli]") {
    const Launch launch = parse({"ssh", "someone@example.test:2222"});
    REQUIRE(launch.kind == Launch::Kind::Adhoc);
    CHECK(launch.profile.backend == BackendKind::Ssh);
    CHECK(launch.profile.host == "example.test");
    CHECK(launch.profile.user == "someone");
    CHECK(launch.profile.port == 2222);
    CHECK_FALSE(launch.profile.id.empty());

    // The OpenSSH flag spellings, and either order means the same thing.
    const Launch flags = parse({"ssh", "example.test", "-p", "2200", "-l", "ops"});
    REQUIRE(flags.kind == Launch::Kind::Adhoc);
    CHECK(flags.profile.port == 2200);
    CHECK(flags.profile.user == "ops");

    const Launch before = parse({"ssh", "-l", "ops", "-p", "2200", "example.test"});
    REQUIRE(before.kind == Launch::Kind::Adhoc);
    CHECK(before.profile.port == 2200);
    CHECK(before.profile.user == "ops");

    // -l overrides user@host rather than being ignored by it.
    const Launch overridden = parse({"ssh", "someone@example.test", "-l", "ops"});
    CHECK(overridden.profile.user == "ops");
}

TEST_CASE("bad arguments fail loudly instead of connecting", "[session][cli]") {
    const std::vector<std::vector<std::string>> bad = {
        {"ssh"},
        {"ssh", "--nope"},
        {"ssh", "host", "extra"},
        {"ssh", "host", "-p"},
        {"ssh", "host", "-p", "nope"},
        {"ssh", "host", "-p", "0"},
        {"ssh", "host", "-p", "70000"},
        {"--nope"},
        {"prod", "extra"},
    };
    for (const std::vector<std::string>& args : bad) {
        const Launch launch = parse(args);
        CAPTURE(args[0]);
        CHECK(launch.kind == Launch::Kind::Message);
        CHECK(launch.error);
        // The usage text comes with the complaint: a bare "unknown option" is
        // an error message that makes the user go looking.
        CHECK(launch.message.find("krait ssh") != std::string::npos);
    }
}

TEST_CASE("help and version are not errors", "[session][cli]") {
    for (const char* flag : {"--help", "-h", "--version"}) {
        const Launch launch = parse({flag});
        CHECK(launch.kind == Launch::Kind::Message);
        CHECK_FALSE(launch.error);
        CHECK_FALSE(launch.message.empty());
    }
}

TEST_CASE("an ad-hoc profile is storable as-is", "[session][cli]") {
    // The point of returning a Profile rather than loose strings: whatever
    // comes off the command line has to be the same shape as everything else,
    // or "save this session" becomes a second conversion with its own bugs.
    ProfileStore store;
    const Launch launch = parse({"ssh", "ops@example.test:2222"});
    REQUIRE(launch.kind == Launch::Kind::Adhoc);

    const std::string id = store.add(launch.profile);
    const Profile* saved = store.find(id);
    REQUIRE(saved != nullptr);
    CHECK(saved->host == "example.test");
    CHECK(saved->user == "ops");
    CHECK(saved->port == 2222);
    CHECK(saved->backend == BackendKind::Ssh);
}
