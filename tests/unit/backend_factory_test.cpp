// T52: Profile -> backend. The mapping is small and entirely made of the kind
// of mistake that is invisible until someone cannot connect: a port that came
// out of a hand-edited TOML, an auth method that shifted by one when an enum
// grew, a vault key built from the name instead of the id.

#include "backend_factory.h"
#include "net/conpty/conpty_backend.h"
#include "net/ssh/ssh_backend.h"
#include <catch2/catch_test_macros.hpp>

#include <QObject>

namespace kapp = krait::app;
namespace kses = krait::app::session;
namespace knet = krait::net;

namespace {

kses::Profile sshProfile() {
    kses::Profile profile;
    profile.id = "web-1-prod";
    profile.name = "Web 1 (prod)";
    profile.backend = kses::BackendKind::Ssh;
    profile.host = "web1.example.com";
    profile.port = 2222;
    profile.user = "deploy";
    profile.auth = kses::SshAuth::PublicKey;
    profile.keyPath = "C:/keys/id_ed25519";
    return profile;
}

}  // namespace

TEST_CASE("sshConfigFor carries the profile across", "[factory]") {
    const knet::SshConfig config = kapp::sshConfigFor(sshProfile());
    CHECK(config.host == "web1.example.com");
    CHECK(config.port == 2222);
    CHECK(config.user == "deploy");
    CHECK(config.keyPath == "C:/keys/id_ed25519");
    CHECK(config.auth == knet::SshAuthPreference::PublicKey);
}

TEST_CASE("sshConfigFor carries a certificate path", "[factory]") {
    // T61. The path is carried, not derived: a certificate that sits beside its
    // key is found by the backend on its own, so a value here always means the
    // CA put it somewhere else. Dropping it would leave the profile looking
    // configured and the connection quietly using the bare key.
    kses::Profile profile = sshProfile();
    CHECK(kapp::sshConfigFor(profile).certPath.empty());
    profile.certPath = "C:/keys/prod-ca-cert.pub";
    CHECK(kapp::sshConfigFor(profile).certPath == "C:/keys/prod-ca-cert.pub");
}

TEST_CASE("a leading ~ in a key or certificate path is expanded", "[factory]") {
    // libssh expands `~` only while applying its OPTIONS, and a key this
    // backend opens itself never goes through there. `~/.ssh/id_ed25519` is the
    // canonical spelling in an ssh_config, so every key T62 imports arrives
    // written that way — and left alone the profile looks configured while the
    // key silently never loads.
    kses::Profile profile = sshProfile();
    profile.keyPath = "~/.ssh/id_ed25519";
    profile.certPath = "~/.ssh/id_ed25519-cert.pub";
    const knet::SshConfig config = kapp::sshConfigFor(profile);
    CHECK(config.keyPath.find('~') == std::string::npos);
    CHECK(config.certPath.find('~') == std::string::npos);
    CHECK(config.keyPath.find("/.ssh/id_ed25519") != std::string::npos);
    CHECK(config.certPath.find("/.ssh/id_ed25519-cert.pub") != std::string::npos);

    // Only a LEADING one, and only as a path segment. A file honestly named
    // with a tilde is not a home directory reference.
    profile.keyPath = "C:/keys/~backup/id_ed25519";
    CHECK(kapp::sshConfigFor(profile).keyPath == "C:/keys/~backup/id_ed25519");
    profile.keyPath = "~notahome/id_ed25519";
    CHECK(kapp::sshConfigFor(profile).keyPath == "~notahome/id_ed25519");
}

TEST_CASE("sshConfigFor keys the vault by id, not name", "[factory]") {
    // profile.h pins the id precisely so a rename does not orphan the stored
    // passphrase. Keying off the name would silently undo that the first time
    // someone renamed a session.
    kses::Profile profile = sshProfile();
    profile.name = "Renamed since";
    CHECK(kapp::sshConfigFor(profile).vaultKey == "web-1-prod");
}

TEST_CASE("sshConfigFor falls back to 22 for a port outside the range", "[factory]") {
    // Profile::port is int64 because TOML hands back int64, and sessions.toml
    // is hand-editable. 65536 truncated to uint16 is 0, which libssh would
    // report as a failure mentioning nothing the user typed.
    kses::Profile profile = sshProfile();
    for (const std::int64_t bad :
         {std::int64_t{0}, std::int64_t{-1}, std::int64_t{65536}, std::int64_t{4294967296}}) {
        profile.port = bad;
        CHECK(kapp::sshConfigFor(profile).port == 22);
    }
    profile.port = 65535;
    CHECK(kapp::sshConfigFor(profile).port == 65535);
}

TEST_CASE("every auth method maps to its own preference", "[factory]") {
    // Not a loop over one pair: the point is that no two profile methods
    // collapse onto the same preference, which is what a mis-ordered switch
    // would do silently.
    kses::Profile profile = sshProfile();
    const std::pair<kses::SshAuth, knet::SshAuthPreference> pairs[] = {
        {kses::SshAuth::Auto, knet::SshAuthPreference::Auto},
        {kses::SshAuth::Agent, knet::SshAuthPreference::Agent},
        {kses::SshAuth::Password, knet::SshAuthPreference::Password},
        {kses::SshAuth::PublicKey, knet::SshAuthPreference::PublicKey},
        {kses::SshAuth::KeyboardInteractive, knet::SshAuthPreference::KeyboardInteractive},
    };
    for (const auto& [from, to] : pairs) {
        profile.auth = from;
        CHECK(kapp::sshConfigFor(profile).auth == to);
    }
}

TEST_CASE("resolveShellCommand pins the default shell to an absolute path", "[factory]") {
    // The empty case is the one that runs for every local tab, so if it ever
    // came back relative, CreateProcessW would search the app directory and the
    // current directory for "powershell.exe" before System32.
    std::wstring exe;
    std::wstring line;
    QString whyNot;
    REQUIRE(knet::resolveShellCommand(QString(), &exe, &line, &whyNot));
    CHECK(whyNot.isEmpty());
    const QString path = QString::fromStdWString(exe);
    CHECK(path.contains(QStringLiteral(":\\")));
    CHECK(path.endsWith(QStringLiteral("powershell.exe"), Qt::CaseInsensitive));
    // Quoted in the command line, which is the documented mitigation for the
    // rest of the search-order problem.
    CHECK(QString::fromStdWString(line).startsWith(QStringLiteral("\"") + path +
                                                   QStringLiteral("\"")));
}

TEST_CASE("resolveShellCommand refuses a command it cannot find", "[factory]") {
    // A profile naming something that is not there must fail with a sentence,
    // not fall back to a shell the user did not ask for.
    std::wstring exe;
    std::wstring line;
    QString whyNot;
    CHECK_FALSE(knet::resolveShellCommand(QStringLiteral("krait-no-such-shell-9f3a.exe"), &exe,
                                          &line, &whyNot));
    CHECK_FALSE(whyNot.isEmpty());
    CHECK(whyNot.contains(QStringLiteral("krait-no-such-shell-9f3a")));
}

TEST_CASE("resolveShellCommand keeps arguments and honours quotes", "[factory]") {
    std::wstring exe;
    std::wstring line;
    QString whyNot;

    // Unquoted: the first space ends the executable.
    REQUIRE(knet::resolveShellCommand(QStringLiteral("cmd.exe /k echo hi"), &exe, &line, &whyNot));
    CHECK(QString::fromStdWString(exe).endsWith(QStringLiteral("cmd.exe"), Qt::CaseInsensitive));
    CHECK(QString::fromStdWString(line).endsWith(QStringLiteral(" /k echo hi")));

    // Quoted: the quotes delimit a path that contains spaces, and the resolved
    // path — not the text the user typed — is what ends up quoted in the line.
    REQUIRE(knet::resolveShellCommand(QStringLiteral("\"cmd.exe\" /k dir"), &exe, &line, &whyNot));
    CHECK(QString::fromStdWString(exe).endsWith(QStringLiteral("cmd.exe"), Qt::CaseInsensitive));
    CHECK(QString::fromStdWString(line).endsWith(QStringLiteral(" /k dir")));

    // Environment variables are expanded before the search, so a profile can
    // say %ComSpec% without naming a drive letter.
    REQUIRE(knet::resolveShellCommand(QStringLiteral("%ComSpec%"), &exe, &line, &whyNot));
    CHECK(QString::fromStdWString(exe).endsWith(QStringLiteral("cmd.exe"), Qt::CaseInsensitive));
}

TEST_CASE("resolveShellCommand does not guess at an unterminated quote", "[factory]") {
    std::wstring exe;
    std::wstring line;
    QString whyNot;
    // `"cmd.exe /k dir` — the quote never closes. Picking a split point would
    // mean launching something the user did not write; the whole remainder is
    // treated as the path instead, which then simply is not found.
    CHECK_FALSE(
        knet::resolveShellCommand(QStringLiteral("\"cmd.exe /k dir"), &exe, &line, &whyNot));
    CHECK_FALSE(whyNot.isEmpty());

    // An unterminated quote around a name that DOES resolve still must not
    // silently swallow the arguments into the executable name.
    CHECK_FALSE(knet::resolveShellCommand(QStringLiteral("\"   "), &exe, &line, &whyNot));
}

TEST_CASE("makeBackend builds the backend the profile names", "[factory]") {
    QObject parent;

    knet::IBackend* ssh = kapp::makeBackend(sshProfile(), nullptr, &parent);
    REQUIRE(ssh != nullptr);
    CHECK(qobject_cast<knet::SshBackend*>(ssh) != nullptr);

    kses::Profile local;
    local.id = "local";
    local.backend = kses::BackendKind::Conpty;
    knet::IBackend* shell = kapp::makeBackend(local, nullptr, &parent);
    REQUIRE(shell != nullptr);
    CHECK(qobject_cast<knet::ConptyBackend*>(shell) != nullptr);

    // Parented, so closing a tab takes its backend with it. A backend that
    // outlived its terminal would keep a worker thread and a socket alive with
    // nothing reading them.
    CHECK(ssh->parent() == &parent);
    CHECK(shell->parent() == &parent);
}
