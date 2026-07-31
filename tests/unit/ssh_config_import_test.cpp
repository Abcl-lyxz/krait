// T62: the OpenSSH ssh_config importer.
//
// Every rule asserted here was read out of ssh_config(5) and OpenSSH's own
// readconf.c before the parser was written, because most of them are the
// opposite of what a first guess produces: the `=` separator, first-value-wins
// rather than last, two arguments for LocalForward rather than one, and
// HostName defaulting to the alias instead of to nothing.

#include "ssh_config_import.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace krait::app::session;

namespace {

const Profile* byName(const SshConfigImport& imported, std::string_view name) {
    const auto it = std::find_if(imported.profiles.begin(), imported.profiles.end(),
                                 [name](const Profile& one) { return one.name == name; });
    return it == imported.profiles.end() ? nullptr : &*it;
}

bool mentions(const std::vector<std::string>& lines, std::string_view needle) {
    return std::any_of(lines.begin(), lines.end(), [needle](const std::string& line) {
        return line.find(needle) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("a plain host becomes a profile", "[sshconfig]") {
    const SshConfigImport imported = importFromSshConfig(R"(
Host web1
    HostName 10.0.0.1
    Port 2222
    User deploy
)");
    REQUIRE(imported.profiles.size() == 1);
    const Profile& web = imported.profiles.front();
    CHECK(web.name == "web1");
    CHECK(web.host == "10.0.0.1");
    CHECK(web.port == 2222);
    CHECK(web.user == "deploy");
    CHECK(web.backend == BackendKind::Ssh);
    // Explicit, so the first save writes them out rather than treating them as
    // inherited and dropping them.
    CHECK(web.isExplicit("host"));
    CHECK(web.isExplicit("port"));
}

TEST_CASE("HostName defaults to the Host alias", "[sshconfig]") {
    // ssh_config(5): the default is the name given on the command line. Leave
    // `host` empty here and the profile cannot connect while looking like it
    // should — and `Host bastion.example.com` with no HostName is an extremely
    // common way to write one.
    const SshConfigImport imported = importFromSshConfig("Host bastion.example.com\n  User ops\n");
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().host == "bastion.example.com");
}

TEST_CASE("the equals separator is a separator", "[sshconfig]") {
    // "separated from their values by whitespace or exactly one = character
    // (which may be surrounded by whitespace)". Read as whitespace-only, `Port
    // = 2222` parses as the argument "= 2222", fails to convert, and silently
    // leaves 22 in place.
    const SshConfigImport imported = importFromSshConfig(R"(
Host a
HostName=10.0.0.9
Port = 2222
User   =   ops
)");
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().host == "10.0.0.9");
    CHECK(imported.profiles.front().port == 2222);
    CHECK(imported.profiles.front().user == "ops");
}

TEST_CASE("keywords fold but arguments do not", "[sshconfig]") {
    const SshConfigImport imported = importFromSshConfig(R"(
HOST a
    hostname Server.Example.COM
    IDENTITYFILE C:/Keys/Id_ED25519
)");
    REQUIRE(imported.profiles.size() == 1);
    // The value keeps its case: a key path is a path, and folding it is how an
    // import breaks against a case-sensitive share.
    CHECK(imported.profiles.front().host == "Server.Example.COM");
    CHECK(imported.profiles.front().keyPath == "C:/Keys/Id_ED25519");
}

TEST_CASE("the FIRST value of a repeated keyword wins", "[sshconfig]") {
    // ssh_config(5)'s central rule, and backwards from how nearly every other
    // config format works. Taking the last value would connect to the wrong
    // host on any file that relies on it.
    const SshConfigImport imported = importFromSshConfig(R"(
Host a
    HostName first.example.com
    HostName second.example.com
    Port 2200
    Port 2201
)");
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().host == "first.example.com");
    CHECK(imported.profiles.front().port == 2200);
}

TEST_CASE("Host * supplies defaults and is not itself a session", "[sshconfig]") {
    const SshConfigImport imported = importFromSshConfig(R"(
Host web1
    HostName 10.0.0.1
Host db1
    HostName 10.0.0.2
    User dba
Host *
    User deploy
    IdentityFile C:/keys/id_ed25519
)");
    REQUIRE(imported.profiles.size() == 2);
    const Profile* web = byName(imported, "web1");
    const Profile* db = byName(imported, "db1");
    REQUIRE(web != nullptr);
    REQUIRE(db != nullptr);
    CHECK(web->user == "deploy");  // from the defaults
    CHECK(db->user == "dba");      // its own wins
    CHECK(web->keyPath == "C:/keys/id_ed25519");
    CHECK(db->keyPath == "C:/keys/id_ed25519");
    // And "*" is not reported as left behind: nothing was lost.
    CHECK_FALSE(mentions(imported.skipped, "*"));
}

TEST_CASE("patterns and multi-host lines are named, not silently dropped", "[sshconfig]") {
    const SshConfigImport imported = importFromSshConfig(R"(
Host prod-*
    User deploy
Host web1 web2
    HostName shared.example.com
Host !badger
    User nobody
Host real
    HostName 10.0.0.3
)");
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().name == "real");
    CHECK(mentions(imported.skipped, "prod-*"));
    CHECK(mentions(imported.skipped, "web1 web2"));
    CHECK(mentions(imported.skipped, "!badger"));
}

TEST_CASE("a Match block is reported and its contents ignored", "[sshconfig]") {
    // Match applies by a rule decided at connect time — exec, localnetwork,
    // canonical. Folding its values into the preceding Host would be worse than
    // dropping them: the profile would carry settings that only apply
    // sometimes, with nothing saying so.
    const SshConfigImport imported = importFromSshConfig(R"(
Host a
    HostName 10.0.0.1
Match host b
    User wrong
    Port 9999
)");
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().user.empty());
    CHECK(imported.profiles.front().port == 22);
    CHECK(mentions(imported.skipped, "Match"));
}

TEST_CASE("forwards take OpenSSH's two-argument spelling", "[sshconfig]") {
    // readconf.c joins the two arguments with a colon and hands the result to
    // the same parser that reads `ssh -L`. Reading LocalForward as one argument
    // would drop the target half of every forward in the file.
    const SshConfigImport imported = importFromSshConfig(R"(
Host tunnel
    HostName 10.0.0.4
    LocalForward 8080 internal:80
    RemoteForward 9090 localhost:90
    DynamicForward 1080
)");
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().forwards == "L 8080:internal:80, R 9090:localhost:90, D 1080");
}

TEST_CASE("forwards do not inherit from the defaults block", "[sshconfig]") {
    // ssh accumulates forwards across every matching block, so copying the
    // Host * forward into each profile would open the same local port once per
    // imported host — and every one after the first would fail with the port
    // already in use.
    const SshConfigImport imported = importFromSshConfig(R"(
Host a
    HostName 10.0.0.1
Host b
    HostName 10.0.0.2
Host *
    DynamicForward 1080
)");
    REQUIRE(imported.profiles.size() == 2);
    CHECK(imported.profiles[0].forwards.empty());
    CHECK(imported.profiles[1].forwards.empty());
}

TEST_CASE("ProxyJump comes across, and none clears it", "[sshconfig]") {
    const SshConfigImport imported = importFromSshConfig(R"(
Host inner
    HostName 10.0.0.5
    ProxyJump me@bastion:2222,second
Host direct
    HostName 10.0.0.6
    ProxyJump none
)");
    const Profile* inner = byName(imported, "inner");
    const Profile* direct = byName(imported, "direct");
    REQUIRE(inner != nullptr);
    REQUIRE(direct != nullptr);
    CHECK(inner->proxyJump == "me@bastion:2222,second");
    // `none` is ssh's way of cancelling an inherited jump. Carried through
    // literally it would try to connect to a host called "none".
    CHECK(direct->proxyJump.empty());
}

TEST_CASE("comments and quotes", "[sshconfig]") {
    const SshConfigImport imported = importFromSshConfig(R"(
# a whole-line comment
Host a   # and one at the end
    HostName 10.0.0.7
    IdentityFile "C:/keys/my key"
)");
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().name == "a");
    CHECK(imported.profiles.front().host == "10.0.0.7");
    CHECK(imported.profiles.front().keyPath == "C:/keys/my key");
}

TEST_CASE("Include is reported rather than followed", "[sshconfig]") {
    // The failure this prevents: a config that keeps every host in
    // ~/.ssh/config.d/ imports as almost nothing and still says "success".
    const SshConfigImport imported = importFromSshConfig(R"(
Include config.d/*.conf
Host a
    HostName 10.0.0.8
)");
    CHECK(imported.profiles.size() == 1);
    REQUIRE(imported.includes.size() == 1);
    CHECK(imported.includes.front() == "config.d/*.conf");
}

TEST_CASE("an empty or junk config imports nothing and does not crash", "[sshconfig]") {
    CHECK(importFromSshConfig("").profiles.empty());
    CHECK(importFromSshConfig("\n\n   \n# just a comment\n").profiles.empty());
    // A keyword with no argument, and a Host line with no pattern.
    CHECK(importFromSshConfig("Host\nHostName\nPort\n").profiles.empty());
    // Settings before any Host line apply to everything, the same as Host * —
    // and on their own they define no session.
    CHECK(importFromSshConfig("User deploy\n").profiles.empty());
}

TEST_CASE("a port outside the wire range is left at the default", "[sshconfig]") {
    // Hand-edited files hold anything. 65536 truncated to a uint16 is 0, which
    // fails with a message naming nothing the user typed.
    const SshConfigImport imported = importFromSshConfig("Host a\n  Port 65536\n");
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().port == 22);
}
