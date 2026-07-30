#include "cli.h"

#include <charconv>
#include <utility>

namespace krait::app::session {

namespace {

constexpr const char* kUsage = "krait — a modern terminal and connection manager\n"
                               "\n"
                               "  krait                          open the default session\n"
                               "  krait <profile>                open a saved profile by name\n"
                               "  krait --profile <name>         the same, unambiguously\n"
                               "  krait ssh [user@]host[:port]   connect without saving a profile\n"
                               "      -p <port>                  port, if not given as host:port\n"
                               "      -l <user>                  user, if not given as user@host\n"
                               "  krait --help | --version\n";

bool wantsValue(std::string_view flag) {
    return flag == "-p" || flag == "-l" || flag == "--profile";
}

bool wholeNumber(std::string_view text, int* out) {
    const char* first = text.data();
    const auto [ptr, ec] = std::from_chars(first, first + text.size(), *out);
    return ec == std::errc{} && ptr == first + text.size();
}

}  // namespace

bool parseTarget(std::string_view target, std::string* user, std::string* host, int* port) {
    user->clear();
    host->clear();

    if (const std::size_t at = target.rfind('@'); at != std::string_view::npos) {
        // rfind, not find: a user name cannot contain '@', and working from the
        // right also copes with the shapes that turn up in the wild.
        *user = std::string(target.substr(0, at));
        target = target.substr(at + 1);
    }

    // Where the port may be, if anywhere. "a number follows the last colon" is
    // NOT the rule: fe80::1 ends in a perfectly good number, and reading it as
    // a port silently connects to a different address on port 1.
    std::size_t portColon = std::string_view::npos;
    if (target.starts_with('[')) {
        // Bracketed IPv6 — the only unambiguous spelling, so it is the only one
        // where a port after a colon-rich host is allowed.
        const std::size_t close = target.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        if (close + 1 < target.size()) {
            if (target[close + 1] != ':') {
                return false;
            }
            portColon = close + 1;
        }
        *host = std::string(target.substr(1, close - 1));
        if (host->empty()) {
            return false;  // "[]" is not a host, with or without a port
        }
        if (portColon == std::string_view::npos) {
            return true;
        }
    } else if (target.find(':') != target.rfind(':')) {
        // More than one colon and no brackets: bare IPv6, so there is no port
        // here to find and the whole thing is the host.
        *host = std::string(target);
        return !host->empty();
    } else {
        portColon = target.find(':');
    }

    if (portColon != std::string_view::npos) {
        const std::string_view tail = target.substr(portColon + 1);
        int parsed = 0;
        if (!wholeNumber(tail, &parsed) || parsed <= 0 || parsed > 65535) {
            // A colon that is not followed by a usable port is a typo worth
            // reporting, not a hostname containing a colon.
            return false;
        }
        *port = parsed;
        if (host->empty()) {
            target = target.substr(0, portColon);
        } else {
            return true;  // bracketed form already set the host
        }
    }

    *host = std::string(target);
    return !host->empty();
}

Launch parseCommandLine(const std::vector<std::string>& args) {
    if (args.size() <= 1) {
        return {};
    }

    const auto fail = [](std::string message) {
        Launch launch;
        launch.kind = Launch::Kind::Message;
        launch.message = std::move(message) + "\n\n" + kUsage;
        launch.error = true;
        return launch;
    };

    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (rest[0] == "--help" || rest[0] == "-h") {
        Launch launch;
        launch.kind = Launch::Kind::Message;
        launch.message = kUsage;
        return launch;
    }
    if (rest[0] == "--version") {
        Launch launch;
        launch.kind = Launch::Kind::Message;
        launch.message = "krait\n";
        return launch;
    }
    if (rest[0] == "--profile") {
        if (rest.size() < 2 || rest[1].empty()) {
            return fail("--profile needs a name");
        }
        Launch launch;
        launch.kind = Launch::Kind::Profile;
        launch.profileName = rest[1];
        return launch;
    }

    if (rest[0] != "ssh") {
        // A single bare word is a PROFILE name. It is deliberately not treated
        // as a host: `krait prod` meaning "connect to the machine literally
        // called prod" is how someone ends up on a box they did not mean, and
        // the profile they wanted is one keystroke away either way.
        if (rest.size() > 1) {
            return fail("unexpected argument: " + rest[1]);
        }
        if (rest[0].starts_with('-')) {
            return fail("unknown option: " + rest[0]);
        }
        Launch launch;
        launch.kind = Launch::Kind::Profile;
        launch.profileName = rest[0];
        return launch;
    }

    Profile profile;
    profile.backend = BackendKind::Ssh;
    profile.markExplicit("backend");
    bool haveTarget = false;

    for (std::size_t i = 1; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (wantsValue(arg)) {
            if (i + 1 >= rest.size()) {
                return fail(arg + " needs a value");
            }
            const std::string& value = rest[++i];
            if (arg == "-p") {
                int port = 0;
                if (!wholeNumber(value, &port) || port <= 0 || port > 65535) {
                    return fail("not a port: " + value);
                }
                profile.port = port;
                profile.markExplicit("port");
            } else if (arg == "-l") {
                profile.user = value;
                profile.markExplicit("user");
            } else {
                return fail("unknown option: " + arg);
            }
            continue;
        }
        if (arg.starts_with('-')) {
            return fail("unknown option: " + arg);
        }
        if (haveTarget) {
            return fail("unexpected argument: " + arg);
        }

        std::string user;
        std::string host;
        int port = static_cast<int>(profile.port);
        if (!parseTarget(arg, &user, &host, &port)) {
            return fail("not a host: " + arg);
        }
        profile.host = host;
        profile.markExplicit("host");
        if (port != profile.port) {
            profile.port = port;
            profile.markExplicit("port");
        }
        // -l after user@host is a normal way to override, and either order
        // should mean the same thing, so the target only fills a user in when
        // it actually carried one.
        if (!user.empty()) {
            profile.user = user;
            profile.markExplicit("user");
        }
        profile.name = arg;
        profile.markExplicit("name");
        haveTarget = true;
    }

    if (!haveTarget) {
        return fail("ssh needs a host");
    }
    profile.id = slugify(profile.name);
    Launch launch;
    launch.kind = Launch::Kind::Adhoc;
    launch.profile = std::move(profile);
    return launch;
}

}  // namespace krait::app::session
