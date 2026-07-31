#pragma once

#include "net/ssh/ssh_backend.h"
#include "session/profile.h"

#include <QObject>

namespace krait::net {
class IBackend;
class Vault;
}  // namespace krait::net

namespace krait::app {

// Profile -> backend (plan T52). The one place a saved session becomes a live
// connection; before it existed the palette could list sessions and nothing
// else, which is what "engine complete, not a product" meant in M2.
//
// A free function rather than a class: it holds no state and has one caller
// shape, and a Factory object with one method is the abstraction rules/cpp.md
// tells us not to write.

// The SSH half, split out because it is the part worth testing without a
// socket: a dropped vault key or a port that came out of a hand-edited TOML as
// 0 is invisible at compile time and fatal at connect time.
net::SshConfig sshConfigFor(const session::Profile& profile);

// Builds the backend `profile` names, parented to `parent` (Qt ownership).
// `vault` is borrowed, outlives the backend, and may be null — every credential
// is then asked for interactively rather than retrieved.
//
// Never returns null. BackendKind is closed on purpose (profile.h), so a TOML
// file naming a backend we do not have has already failed at load.
net::IBackend* makeBackend(const session::Profile& profile, net::Vault* vault, QObject* parent);

}  // namespace krait::app
