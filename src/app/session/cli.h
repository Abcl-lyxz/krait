#pragma once

#include "profile.h"

#include <string>
#include <string_view>
#include <vector>

namespace krait::app::session {

// What the command line asked the app to open.
//
// Parsing is separated from acting for the usual reason — every interesting
// decision is in the parse, and a test for it must not need a window — but also
// because the answer to "what does `krait prod` mean" depends on the profile
// store, and resolving that inside an argv loop would tie the two together for
// good.
struct Launch {
    enum class Kind {
        // No arguments: the configured default, which is a local shell.
        Default,
        // A saved profile, by name or id.
        Profile,
        // Connection details given on the command line with no profile behind
        // them. Held as a Profile so everything downstream takes one shape.
        Adhoc,
        // --help / --version, or a parse error. `message` is what to print;
        // `error` says whether it goes to stderr and exits non-zero.
        Message,
    };

    Kind kind = Kind::Default;
    std::string profileName;  // Kind::Profile
    Profile profile;          // Kind::Adhoc
    std::string message;      // Kind::Message
    bool error = false;
};

// `args[0]` is expected to be present and is ignored.
//
// Accepted, and deliberately no more than this:
//   krait                          the default session
//   krait ssh [user@]host[:port]   an ad-hoc connection
//   krait ssh ... -p PORT          port as a flag, PuTTY/OpenSSH spelling
//   krait ssh ... -l USER          user as a flag, OpenSSH spelling
//   krait <profile>                a saved profile by name
//   krait --profile NAME           the same, unambiguously
//   krait --help | --version
//
// A bare `krait somehost` is NOT an ad-hoc connection: it is a profile lookup.
// An argument that is neither a known profile nor an explicit `ssh` target
// should fail loudly rather than silently dial a host the user did not mean.
Launch parseCommandLine(const std::vector<std::string>& args);

// user@host:port -> the three parts. Returns false when the text cannot be one:
// an empty host, or a port that is not a number in range.
bool parseTarget(std::string_view target, std::string* user, std::string* host, int* port);

}  // namespace krait::app::session
