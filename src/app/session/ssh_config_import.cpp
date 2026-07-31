#include "ssh_config_import.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <utility>

namespace krait::app::session {

namespace {

// ssh_config(5): "keywords are case-insensitive and arguments are
// case-sensitive". So only the keyword is folded, and never the value — an
// IdentityFile path has to survive intact.
std::string foldKeyword(std::string_view text) {
    std::string folded;
    folded.reserve(text.size());
    for (const char raw : text) {
        folded += static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
    }
    return folded;
}

bool isSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// One line, split into keyword and the rest.
//
// ssh_config(5): separated "by whitespace or exactly one `=` character (which
// may be surrounded by whitespace)". Both spellings appear in real files, and a
// parser that only knows the space form reads `Port = 2222` as a keyword with
// the argument "= 2222", which then fails to parse as a number and silently
// leaves the default in place.
bool splitDirective(std::string_view line, std::string* keyword, std::string_view* rest) {
    // A '#' outside quotes starts a comment, including mid-line.
    bool inQuotes = false;
    for (std::size_t at = 0; at < line.size(); ++at) {
        if (line[at] == '"') {
            inQuotes = !inQuotes;
        } else if (line[at] == '#' && !inQuotes) {
            line = line.substr(0, at);
            break;
        }
    }
    line = trim(line);
    if (line.empty()) {
        return false;
    }

    std::size_t end = 0;
    while (end < line.size() && !isSpace(line[end]) && line[end] != '=') {
        ++end;
    }
    *keyword = foldKeyword(line.substr(0, end));
    std::string_view tail = trim(line.substr(end));
    if (!tail.empty() && tail.front() == '=') {
        tail = trim(tail.substr(1));
    }
    *rest = tail;
    return !keyword->empty();
}

// Whitespace-separated arguments, honouring double quotes. ssh_config(5)
// documents the quotes and says nothing at all about backslash escapes, so none
// are implemented — inventing an unescape rule the format may not have is how a
// Windows path full of backslashes gets quietly mangled.
std::vector<std::string> splitArguments(std::string_view text) {
    std::vector<std::string> args;
    std::string current;
    bool inQuotes = false;
    bool started = false;
    for (const char raw : text) {
        if (raw == '"') {
            inQuotes = !inQuotes;
            started = true;
            continue;
        }
        if (isSpace(raw) && !inQuotes) {
            if (started) {
                args.push_back(std::move(current));
                current.clear();
                started = false;
            }
            continue;
        }
        current += raw;
        started = true;
    }
    if (started) {
        args.push_back(std::move(current));
    }
    return args;
}

// A `Host` argument that is a matcher rather than a name. `*` and `?` are the
// documented wildcards and `!` is negation; none of the three can become a
// profile, because there is no host to connect to.
bool isPattern(std::string_view name) {
    return name.find('*') != std::string_view::npos || name.find('?') != std::string_view::npos ||
           (!name.empty() && name.front() == '!');
}

// One Host block, before it is known whether it is a real host or the defaults.
struct Block {
    std::vector<std::string> patterns;
    std::vector<std::pair<std::string, std::string>> values;
    std::vector<std::string> forwards;

    // ssh_config(5)'s first-obtained-value rule, within one block: a keyword
    // repeated in the same block keeps its FIRST value, not its last.
    void set(const std::string& keyword, std::string value) {
        for (const auto& [name, ignored] : values) {
            if (name == keyword) {
                return;
            }
        }
        values.emplace_back(keyword, std::move(value));
    }

    const std::string* get(std::string_view keyword) const {
        for (const auto& [name, value] : values) {
            if (name == keyword) {
                return &value;
            }
        }
        return nullptr;
    }
};

// LocalForward and RemoteForward take TWO whitespace-separated arguments, which
// OpenSSH's own readconf.c joins with a colon before handing them to the same
// parser that reads `ssh -L`. DynamicForward takes one. Krait spells a forward
// with the letter in front, so that is all that is left to do.
void addForward(Block* block, char letter, const std::vector<std::string>& args) {
    if (args.empty()) {
        return;
    }
    std::string spec(1, letter);
    spec += ' ';
    spec += args[0];
    if (args.size() >= 2) {
        spec += ':';
        spec += args[1];
    } else if (letter == 'L') {
        // -L with no target is not a forward at all. -R with one argument is
        // the remote-dynamic form, and -D never has a second.
        return;
    }
    block->forwards.push_back(std::move(spec));
}

std::string joinForwards(const std::vector<std::string>& forwards) {
    std::string joined;
    for (const std::string& one : forwards) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += one;
    }
    return joined;
}

// Copies one value in and marks it explicit, so a later save writes it out
// rather than mistaking it for something inherited.
void take(Profile* profile, std::string* field, const std::string* value, const char* key) {
    if (value == nullptr || value->empty()) {
        return;
    }
    *field = *value;
    profile->markExplicit(key);
}

}  // namespace

SshConfigImport importFromSshConfig(std::string_view text) {
    SshConfigImport result;

    std::vector<Block> blocks;
    Block defaults;
    bool inMatch = false;

    const auto readLine = [&](std::string_view line) {
        std::string keyword;
        std::string_view rest;
        if (!splitDirective(line, &keyword, &rest)) {
            return;
        }

        if (keyword == "host") {
            inMatch = false;
            Block block;
            block.patterns = splitArguments(rest);
            blocks.push_back(std::move(block));
            return;
        }
        if (keyword == "match") {
            // A Match block applies by a rule this importer does not model —
            // exec, localnetwork, canonical and the rest are decided at connect
            // time, not at import time. Everything inside one is dropped, and
            // the block is named so the count is not a count the user has to
            // reconcile by hand.
            inMatch = true;
            result.skipped.push_back("Match " + std::string(rest) + " (conditional block)");
            return;
        }
        if (keyword == "include") {
            result.includes.emplace_back(rest);
            return;
        }
        if (inMatch) {
            return;
        }

        const std::vector<std::string> args = splitArguments(rest);
        if (args.empty()) {
            return;
        }
        if (blocks.empty()) {
            // Before any Host line, ssh_config applies to everything — the same
            // role `Host *` plays, so it is treated as those defaults.
            defaults.set(keyword, args.front());
            return;
        }

        Block& block = blocks.back();
        if (keyword == "localforward") {
            addForward(&block, 'L', args);
        } else if (keyword == "remoteforward") {
            addForward(&block, 'R', args);
        } else if (keyword == "dynamicforward") {
            addForward(&block, 'D', args);
        } else {
            block.set(keyword, args.front());
        }
    };

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        readLine(text.substr(start, end - start));
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }

    // `Host *` is how a file spells "these apply to everything", so its values
    // join the pre-Host defaults rather than becoming a profile of their own.
    //
    // NOT modelled: ssh_config's first-obtained-value rule ACROSS blocks, which
    // makes a `Host *` placed before the specific blocks beat them. Files are
    // written the other way round — the manual page itself says to put general
    // defaults at the end — and modelling it properly means running the pattern
    // match per keyword per host.
    for (const Block& block : blocks) {
        if (block.patterns.size() == 1 && block.patterns.front() == "*") {
            for (const auto& [keyword, value] : block.values) {
                defaults.set(keyword, value);
            }
        }
    }

    for (const Block& block : blocks) {
        if (block.patterns.empty()) {
            continue;
        }
        if (block.patterns.size() > 1) {
            std::string names;
            for (const std::string& one : block.patterns) {
                if (!names.empty()) {
                    names += ' ';
                }
                names += one;
            }
            result.skipped.push_back(names + " (one Host line, several hosts)");
            continue;
        }
        const std::string& alias = block.patterns.front();
        if (isPattern(alias)) {
            // `*` is the defaults block, already folded in above, and saying it
            // was "skipped" would read as something being lost.
            if (alias != "*") {
                result.skipped.push_back(alias + " (a pattern, not a host)");
            }
            continue;
        }

        Profile profile;
        profile.backend = BackendKind::Ssh;
        profile.markExplicit("backend");
        profile.name = alias;
        profile.markExplicit("name");
        profile.id = slugify(alias);

        // ssh_config(5): HostName defaults to the name given on the command
        // line, which for an imported block is the alias itself. Leaving `host`
        // empty would produce a profile that cannot connect and looks like it
        // should.
        const std::string* hostName = block.get("hostname");
        profile.host = hostName != nullptr && !hostName->empty() ? *hostName : alias;
        profile.markExplicit("host");

        const auto valueOf = [&](std::string_view keyword) {
            const std::string* own = block.get(keyword);
            return own != nullptr ? own : defaults.get(keyword);
        };

        take(&profile, &profile.user, valueOf("user"), "user");
        take(&profile, &profile.keyPath, valueOf("identityfile"), "key_path");
        take(&profile, &profile.certPath, valueOf("certificatefile"), "cert_path");
        take(&profile, &profile.proxyJump, valueOf("proxyjump"), "proxy_jump");

        if (const std::string* port = valueOf("port"); port != nullptr) {
            std::int64_t parsed = 0;
            std::from_chars(port->data(), port->data() + port->size(), parsed);
            if (parsed > 0 && parsed <= 65535) {
                profile.port = parsed;
                profile.markExplicit("port");
            }
        }
        // `ProxyJump none` is ssh's way of cancelling an inherited one. Carried
        // through, it would send the connection to a host called "none".
        if (profile.proxyJump == "none") {
            profile.proxyJump.clear();
        }

        // Forwards do NOT inherit from the defaults block. ssh accumulates them
        // across every matching block, so pulling `Host *`'s forwards into
        // every profile would open the same tunnel once per imported host, and
        // all but the first would fail with the port already in use.
        if (!block.forwards.empty()) {
            profile.forwards = joinForwards(block.forwards);
            profile.markExplicit("forwards");
        }

        result.profiles.push_back(std::move(profile));
    }

    return result;
}

std::string defaultSshConfigPath() {
    // USERPROFILE, not HOME: on Windows the OpenSSH client reads ~/.ssh from
    // the profile directory, and HOME is usually only set inside an MSYS or WSL
    // shell — where it points somewhere else entirely.
    std::size_t needed = 0;
    if (::getenv_s(&needed, nullptr, 0, "USERPROFILE") != 0 || needed <= 1) {
        return {};
    }
    std::string home(needed, '\0');
    if (::getenv_s(&needed, home.data(), home.size(), "USERPROFILE") != 0) {
        return {};
    }
    home.resize(needed > 0 ? needed - 1 : 0);
    if (home.empty()) {
        return {};
    }
    return home + "\\.ssh\\config";
}

}  // namespace krait::app::session
