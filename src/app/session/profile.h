#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace krait::app::session {

// Which backend a profile drives. The enum is
// closed on purpose so a TOML file naming a backend we do not have fails
// loudly at load rather than opening a shell instead of a connection.
enum class BackendKind { Conpty, Ssh, Telnet, Raw, Serial };

// Auth METHOD, not credentials — nothing here is ever a secret. Passwords and
// passphrases live in the DPAPI vault keyed by the profile id (rules/net.md).
enum class SshAuth { Auto, Agent, Password, PublicKey, KeyboardInteractive };

std::string backendName(BackendKind kind);
std::string authName(SshAuth auth);
// Unknown text falls back to the safe default rather than guessing; `parseOk`
// says which happened, so load() can warn instead of silently rewriting.
BackendKind parseBackend(std::string_view text, bool* parseOk = nullptr);
SshAuth parseAuth(std::string_view text, bool* parseOk = nullptr);

// One saved connection. Plain data: it crosses no thread and owns no handle,
// so the session tree, the palette, the importer and the backend factory can
// all hold copies without a lifetime question between them.
struct Profile {
    // Stable key, and the vault key prefix. Derived from the name on first
    // save and then PINNED in the file, so a rename does not orphan a stored
    // passphrase.
    std::string id;
    std::string name;
    // "prod/eu" — the tree is derived from this string rather than stored as
    // one. A tree in the file is a tree to keep consistent on every move.
    std::string folder;
    std::vector<std::string> tags;
    BackendKind backend = BackendKind::Conpty;

    std::string host;
    std::int64_t port = 22;
    std::string user;
    SshAuth auth = SshAuth::Auto;
    std::string keyPath;
    // OpenSSH's ProxyJump spelling: "bastion" or "me@bastion:2222,inner".
    // Empty means a direct connection.
    std::string proxyJump;

    // rules/ui.md: safety accents (prod = red) are a core UX invariant, never
    // behind an "advanced" toggle. Empty means the theme decides.
    std::string accent;

    // ConPTY only: the shell to spawn. Empty means the configured default.
    std::string command;

    // Serial only. The PORT is `host`, the way PuTTY stores a serial line in
    // its host field — a profile has one "where", and a second field for it
    // would mean every importer and every editor learning about both.
    std::int64_t baud = 115200;

    // Which keys this profile set ITSELF, as opposed to inheriting from
    // [defaults] or a [folders."..."] table. Save writes only these, so the
    // first save does not flatten a hand-written config into N copies of the
    // same inherited value.
    std::vector<std::string> explicitKeys;

    bool isExplicit(std::string_view key) const;
    void markExplicit(std::string_view key);
};

// "Web 1 (prod)" -> "web-1-prod". Lowercase, runs of non-alphanumerics
// collapsed to one '-', ends trimmed. Empty input yields "session".
std::string slugify(std::string_view name);

// Every folder prefix of `folder`, outermost first: "prod/eu/db" gives
// {"", "prod", "prod/eu", "prod/eu/db"}. The order IS the inheritance order.
std::vector<std::string> folderChain(std::string_view folder);

// Profiles on disk (sessions.toml), plus the [defaults] and [folders."..."]
// tables they inherit from. Errors are values: a config file is user input, so
// a broken one degrades to "no sessions" rather than aborting.
class ProfileStore {
  public:
    // A missing file is a first run, not a failure — `true` either way. False
    // only when the file exists and cannot be parsed; `error()` says why.
    bool load(const std::string& path);
    // Writes the file back, preserving [defaults] and [folders], and per
    // profile only the keys it owns.
    bool save() const;

    const std::string& path() const { return m_path; }

    const std::string& error() const { return m_error; }

    const std::vector<Profile>& profiles() const { return m_profiles; }

    // Adds a profile, assigning an id if it has none and de-duplicating one
    // that collides. Returns the id actually used.
    std::string add(Profile profile);
    bool remove(std::string_view id);
    const Profile* find(std::string_view id) const;

    // Applies `field = value` to every id in `ids` and marks it explicit on
    // each — the bulk edit the milestone asks for. An unknown field returns
    // false and changes nothing, so a typo cannot half-apply across twenty
    // hosts.
    bool bulkSet(const std::vector<std::string>& ids, std::string_view field,
                 std::string_view value);

    // Distinct folder paths across all profiles, sorted, including intermediate
    // ones that hold no profile of their own ("prod" when only "prod/eu" is
    // used) — otherwise the tree has holes in it.
    std::vector<std::string> folders() const;

    // Sets an inherited value without a file, for the tests and the importer.
    void setInherited(std::string_view folder, std::string_view key, std::string_view value);

    // Applies [defaults] and the folder chain to `raw`, then re-applies the
    // keys `raw` owns. Public because the importer builds profiles that never
    // touched a file.
    Profile resolve(const Profile& raw) const;

  private:
    std::string m_path;
    std::string m_error;
    std::vector<Profile> m_profiles;
    // Folder path ("" = [defaults]) -> key -> value, as text. Text because
    // these tables are copied straight back out on save; only the resolved
    // Profile needs real types.
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>
        m_inherited;
};

}  // namespace krait::app::session
