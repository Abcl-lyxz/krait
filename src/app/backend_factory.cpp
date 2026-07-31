#include "backend_factory.h"

#include "net/conpty/conpty_backend.h"
#include "net/ibackend.h"
#include "net/telnet/telnet_backend.h"

#include <QString>

#include <utility>

namespace krait::app {
namespace {

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
    config.keyPath = profile.keyPath;
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
        config.host = profile.host;
        // 23, not 22: the same out-of-range guard as SSH, with telnet's default.
        config.port =
            profile.port > 0 && profile.port <= 65535 ? static_cast<int>(profile.port) : 23;
        // Telnet has no credentials of its own — whatever the far end asks for
        // goes through the terminal like any other output — so no vault key.
        return new net::TelnetBackend(std::move(config), parent);  // owned by parent
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
