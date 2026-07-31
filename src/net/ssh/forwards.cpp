#include "forwards.h"

#include <charconv>

namespace krait::net {
namespace {

// Splits on ':' but respects [..] so an IPv6 literal survives. ssh_config
// brackets them for exactly this reason, and a splitter that does not know
// about brackets turns "[::1]:80" into five empty fields.
std::vector<std::string> splitFields(std::string_view spec) {
    std::vector<std::string> fields;
    std::string current;
    bool inBrackets = false;
    for (const char ch : spec) {
        if (ch == '[') {
            inBrackets = true;
            continue;  // the brackets are syntax, not part of the address
        }
        if (ch == ']') {
            inBrackets = false;
            continue;
        }
        if (ch == ':' && !inBrackets) {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current += ch;
    }
    fields.push_back(current);
    return fields;
}

bool parsePort(std::string_view text, int* out) {
    int value = 0;
    const char* first = text.data();
    const auto [ptr, error] = std::from_chars(first, first + text.size(), value);
    // The WHOLE field has to be the number. "80x" parsing as 80 would forward a
    // port nobody wrote, and a trailing character is far more likely to be a
    // typo in the host than a port the user meant.
    if (error != std::errc{} || ptr != first + text.size()) {
        return false;
    }
    if (value <= 0 || value > 65535) {
        return false;
    }
    *out = value;
    return true;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

bool parseForward(ForwardKind kind, std::string_view spec, Forward* out) {
    const std::vector<std::string> fields = splitFields(trim(spec));
    Forward forward;
    forward.kind = kind;

    if (kind == ForwardKind::Dynamic) {
        // [bind:]port
        if (fields.size() == 1) {
            if (!parsePort(fields[0], &forward.bindPort)) {
                return false;
            }
        } else if (fields.size() == 2) {
            forward.bindAddress = fields[0];
            if (!parsePort(fields[1], &forward.bindPort)) {
                return false;
            }
        } else {
            return false;
        }
        *out = forward;
        return true;
    }

    // [bind:]port:host:hostport
    if (fields.size() == 3) {
        if (!parsePort(fields[0], &forward.bindPort)) {
            return false;
        }
        forward.destHost = fields[1];
        if (!parsePort(fields[2], &forward.destPort)) {
            return false;
        }
    } else if (fields.size() == 4) {
        forward.bindAddress = fields[0];
        if (!parsePort(fields[1], &forward.bindPort)) {
            return false;
        }
        forward.destHost = fields[2];
        if (!parsePort(fields[3], &forward.destPort)) {
            return false;
        }
    } else {
        return false;
    }
    if (forward.destHost.empty()) {
        return false;  // nowhere to send it
    }
    *out = forward;
    return true;
}

std::vector<Forward> parseForwards(std::string_view text, std::vector<std::string>* rejected) {
    std::vector<Forward> forwards;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view entry = trim(text.substr(start, end - start));
        if (comma == std::string_view::npos) {
            start = text.size() + 1;
        } else {
            start = comma + 1;
        }
        if (entry.empty()) {
            continue;
        }

        ForwardKind kind = ForwardKind::Local;
        switch (entry.front()) {
        case 'L':
        case 'l':
            kind = ForwardKind::Local;
            break;
        case 'R':
        case 'r':
            kind = ForwardKind::Remote;
            break;
        case 'D':
        case 'd':
            kind = ForwardKind::Dynamic;
            break;
        default:
            if (rejected != nullptr) {
                rejected->emplace_back(entry);
            }
            continue;
        }

        Forward forward;
        if (parseForward(kind, trim(entry.substr(1)), &forward)) {
            forwards.push_back(std::move(forward));
        } else if (rejected != nullptr) {
            // Named, not merely counted. A tunnel that silently did not open is
            // indistinguishable from a server that is down, and the user will
            // spend the afternoon on the wrong one.
            rejected->emplace_back(entry);
        }
    }
    return forwards;
}

std::string Forward::describe() const {
    const char letter = kind == ForwardKind::Local ? 'L' : kind == ForwardKind::Remote ? 'R' : 'D';
    std::string out;
    out += letter;
    out += ' ';
    if (!bindAddress.empty()) {
        out += bindAddress;
        out += ':';
    }
    out += std::to_string(bindPort);
    if (kind == ForwardKind::Dynamic) {
        out += " -> SOCKS";
        return out;
    }
    out += " -> ";
    out += destHost;
    out += ':';
    out += std::to_string(destPort);
    return out;
}

}  // namespace krait::net
