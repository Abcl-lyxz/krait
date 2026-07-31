#pragma once

#include "profile.h"

#include <string>
#include <string_view>
#include <vector>

namespace krait::app::session {

// Imports OpenSSH's ~/.ssh/config.
//
// Worth saying why this exists at all, because libssh already READS that file:
// ssh_connect() calls ssh_options_parse_config() on its own, so a Krait session
// pointed at a host already inherits whatever ssh_config says about it. What it
// does not do is make those hosts VISIBLE — they are not in the session tree,
// the palette cannot find them, none of them carries a folder or an accent.
// That is what importing buys, and it is why an imported profile names the
// values itself rather than leaning on that inheritance.
//
// The parse is separated from the file read for the same reason the PuTTY
// importer separates its registry read: every interesting decision is in the
// mapping, and a mapping tested against text needs nobody's ~/.ssh.
struct SshConfigImport {
    std::vector<Profile> profiles;
    // Host lines understood and deliberately not turned into a profile, with
    // the reason attached — a pattern rather than a name, or a Match block.
    // Named rather than counted, so someone reading the summary can tell
    // whether the host they were looking for is in it.
    std::vector<std::string> skipped;
    // `Include` directives, verbatim. NOT followed: resolving one means glob
    // expansion against the filesystem, and the relative-path rule differs
    // between a user config and the system one. Reported rather than ignored,
    // because a config that keeps its hosts in an included directory would
    // otherwise import as almost nothing and still look like it worked.
    std::vector<std::string> includes;
};

// Parses the TEXT of an ssh_config. Never touches the filesystem.
SshConfigImport importFromSshConfig(std::string_view text);

// ~/.ssh/config, or "" when there is no home directory to build it from.
std::string defaultSshConfigPath();

}  // namespace krait::app::session
