#include "backend_factory.h"

#include "net/conpty/conpty_backend.h"
#include "net/ibackend.h"
#include "net/raw/raw_backend.h"
#include "net/serial/serial_backend.h"
#include "net/ssh/forwards.h"
#include "net/telnet/telnet_backend.h"

#include <QDir>
#include <QString>

#include <utility>

namespace krait::app {
namespace {

// Turns a leading `~/` into the user's home directory.
//
// libssh does NOT do this for a path handed to ssh_pki_import_privkey_file or
// ssh_pki_import_cert_file. It expands `~` and `%d` only while applying its
// OPTIONS — options.c runs ssh_path_expand_escape over the identity and
// certificate lists inside ssh_options_apply — and a key this backend opens
// itself never goes through there.
//
// That matters because `~/.ssh/id_ed25519` is the canonical spelling in an
// ssh_config, so every key the T62 importer brings across arrives written that
// way. Left alone the profile looks configured and the key never loads, and it
// fails SILENTLY: the import returns SSH_EOF rather than SSH_ERROR, so the
// passphrase path is skipped and Auto quietly degrades to a password prompt.
//
// Done here rather than in the importer so it also covers a profile somebody
// typed the same way by hand, and so sessions.toml keeps the shorter spelling.
std::string expandHome(const std::string& path) {
    if (path.rfind("~/", 0) != 0 && path.rfind("~\\", 0) != 0) {
        return path;
    }
    const QString home = QDir::homePath();
    if (home.isEmpty()) {
        return path;
    }
    return home.toStdString() + "/" + path.substr(2);
}

// The two enums are deliberately separate (ssh_backend.h says why: src/net must
// not depend on the app layer), so this is the seam that keeps them honest. A
// switch rather than a cast: adding a method to either enum has to break the
// build here rather than silently map to whatever shares an integer value.
net::SshAuthPreference authFor(session::SshAuth auth) {
    switch (auth) {
    case session::SshAuth::Auto:
        return net::SshAuthPreference::Auto;
    case session::SshAuth::Agent:
        return net::SshAuthPreference::Agent;
    case session::SshAuth::Password:
        return net::SshAuthPreference::Password;
    case session::SshAuth::PublicKey:
        return net::SshAuthPreference::PublicKey;
    case session::SshAuth::KeyboardInteractive:
        return net::SshAuthPreference::KeyboardInteractive;
    }
    return net::SshAuthPreference::Auto;
}

}  // namespace

net::TcpConfig tcpConfigFor(const session::Profile& profile, int defaultPort) {
    net::TcpConfig config;
    config.host = profile.host;
    // The same out-of-range guard as SSH: sessions.toml is hand-editable, and
    // 65536 truncated to uint16 is 0, which fails with a message about nothing
    // the user wrote.
    config.port =
        profile.port > 0 && profile.port <= 65535 ? static_cast<int>(profile.port) : defaultPort;
    // Matching SshConfig's default rather than leaving the machinery
    // unreachable. It only ever retries CONNECT failures for these backends —
    // a close after a successful connect is reported as a clean end, because
    // nothing at the TCP layer distinguishes a logout from a drop.
    config.maxReconnectAttempts = 5;
    return config;
}

net::SshConfig sshConfigFor(const session::Profile& profile) {
    net::SshConfig config;
    config.host = profile.host;
    // Profile::port is int64 because that is what TOML hands back, and a
    // hand-edited file can hold any of it. Anything outside the port range is
    // the default rather than a truncation: 65536 arriving at libssh as 0 would
    // fail with a message about nothing the user wrote.
    config.port = profile.port > 0 && profile.port <= 65535 ? static_cast<int>(profile.port) : 22;
    config.user = profile.user;
    config.auth = authFor(profile.auth);
    config.keyPath = expandHome(profile.keyPath);
    config.certPath = expandHome(profile.certPath);
    config.proxyJump = profile.proxyJump;
    // Rejected specs are dropped here rather than reported, because makeBackend
    // has no banner to reach. parseForwards names them for a caller that does;
    // wiring that through is what the settings UI will want.
    config.forwards = net::parseForwards(profile.forwards, nullptr);
    // The id, not the name: profile.h pins the id precisely so a rename does
    // not orphan the stored passphrase, and that promise is kept here or
    // nowhere.
    config.vaultKey = profile.id;
    return config;
}

net::IBackend* makeBackend(const session::Profile& profile, net::Vault* vault, QObject* parent) {
    switch (profile.backend) {
    case session::BackendKind::Ssh:
        return new net::SshBackend(sshConfigFor(profile), vault, parent);  // owned by parent
    case session::BackendKind::Telnet: {
        net::TelnetConfig config;
        // Telnet has no credentials of its own — whatever the far end asks for
        // goes through the terminal like any other output — so no vault key.
        config.tcp = tcpConfigFor(profile, 23);
        return new net::TelnetBackend(std::move(config), parent);  // owned by parent
    }
    case session::BackendKind::Raw:
        // No default port: a raw socket is always aimed somewhere specific, and
        // inventing one would connect to a service the user never named.
        return new net::RawBackend(tcpConfigFor(profile, 0), parent);  // owned by parent
    case session::BackendKind::Serial: {
        net::SerialConfig config;
        // `host` IS the port name for a serial profile — PuTTY's convention,
        // and the reason no second field exists for it.
        config.port = profile.host;
        config.baud = profile.baud > 0 ? static_cast<int>(profile.baud) : 115200;
        return new net::SerialBackend(std::move(config), parent);  // owned by parent
    }
    case session::BackendKind::Conpty: {
        auto* backend = new net::ConptyBackend(parent);  // owned by parent
        backend->setCommand(QString::fromStdString(profile.command));
        return backend;
    }
    }
    // Reached only for a value outside the enum, and deliberately without a
    // `default:` label so that adding a backend breaks this switch at COMPILE
    // time. A local shell is the safe landing: it connects to nothing.
    return new net::ConptyBackend(parent);  // owned by parent
}

}  // namespace krait::app
