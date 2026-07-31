#include "core/parser/osc.h"

#include <utility>

namespace krait::core::vt {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char ch) {
    const std::size_t at = kAlphabet.find(ch);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

// Splits at the first `sep`, handing the remainder back through `rest`. OSC
// parameter strings are positional, and a URI legitimately contains characters
// that look like separators, so only the leading fields are split this way.
std::string_view upTo(std::string_view text, char sep, std::string_view* rest) {
    const std::size_t at = text.find(sep);
    if (at == std::string_view::npos) {
        *rest = {};
        return text;
    }
    *rest = text.substr(at + 1);
    return text.substr(0, at);
}

}  // namespace

void OscHandler::start() noexcept {
    m_buffer.clear();
    m_overflowed = false;
}

void OscHandler::put(std::uint8_t byte) {
    if (m_buffer.size() >= kMaxPayload) {
        // Keep consuming so the parser state machine still finds its
        // terminator, but stop growing and remember why.
        m_overflowed = true;
        return;
    }
    m_buffer += static_cast<char>(byte);
}

OscAction OscHandler::end(bool aborted) {
    if (aborted || m_overflowed || m_buffer.empty()) {
        return {};
    }

    std::string_view rest;
    const std::string_view code = upTo(m_buffer, ';', &rest);

    if (code == "0" || code == "2") {
        // A title is remote text that ends up in window chrome. The core hands
        // it over as-is and the app sanitises it — the same boundary every
        // other remote string crosses.
        OscAction action;
        action.kind = OscAction::Kind::Title;
        action.text = std::string(rest);
        return action;
    }

    if (code == "8") {
        // OSC 8 ; params ; URI. An empty URI closes the current link.
        std::string_view uri;
        const std::string_view params = upTo(rest, ';', &uri);

        OscAction action;
        action.kind = OscAction::Kind::Hyperlink;
        action.text = std::string(uri);
        // id= is the only parameter anyone uses. Unknown ones are ignored
        // rather than rejected: the spec says a terminal must skip what it does
        // not recognise.
        std::string_view remaining = params;
        while (!remaining.empty()) {
            std::string_view tail;
            const std::string_view part = upTo(remaining, ':', &tail);
            if (part.starts_with("id=")) {
                action.id = std::string(part.substr(3));
            }
            remaining = tail;
        }
        return action;
    }

    if (code == "52") {
        // OSC 52 ; selection ; data. `?` asks to READ.
        std::string_view data;
        const std::string_view selection = upTo(rest, ';', &data);

        OscAction action;
        action.selection = std::string(selection);
        if (data == "?") {
            action.kind = OscAction::Kind::ClipboardRead;
            return action;
        }
        std::string decoded;
        if (!decodeBase64(data, &decoded, kMaxClipboardBytes)) {
            // Not valid base64, or too big. Refused entirely: a clipboard that
            // is half of what the application sent is worse than one that did
            // not change at all.
            return {};
        }
        action.kind = OscAction::Kind::ClipboardWrite;
        action.text = std::move(decoded);
        return action;
    }

    // Everything else is honest silence. OSC 4 (palette), 10/11 (fg/bg), 7
    // (cwd) and 133 (shell integration) are later milestones, and acting on
    // them now would claim behavior that does not exist.
    return {};
}

bool decodeBase64(std::string_view input, std::string* out, std::size_t maxBytes) {
    out->clear();
    // Padding is optional in the wild, but the LENGTH still has to be a shape
    // base64 can produce. A stray single trailing character encodes nothing and
    // means the sender is confused or hostile.
    while (!input.empty() && input.back() == '=') {
        input.remove_suffix(1);
    }
    if (input.size() % 4 == 1) {
        return false;
    }
    if (input.size() / 4 * 3 > maxBytes) {
        return false;
    }

    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char ch : input) {
        const int value = base64Value(ch);
        if (value < 0) {
            // Deliberately strict: no skipping whitespace or newlines. Being
            // lenient here means two terminals disagree about what a payload
            // decodes to, and clipboard contents are exactly the thing that
            // must not silently differ between them.
            return false;
        }
        accumulator = accumulator << 6 | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>(accumulator >> bits & 0xFF));
            if (out->size() > maxBytes) {
                out->clear();
                return false;
            }
        }
    }
    return true;
}

std::string encodeBase64(std::string_view input) {
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);

    std::size_t i = 0;
    for (; i + 2 < input.size(); i += 3) {
        const std::uint32_t triple = static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(input[i]) << 16 |
            static_cast<std::uint8_t>(input[i + 1]) << 8 | static_cast<std::uint8_t>(input[i + 2]));
        out += kAlphabet[triple >> 18 & 0x3F];
        out += kAlphabet[triple >> 12 & 0x3F];
        out += kAlphabet[triple >> 6 & 0x3F];
        out += kAlphabet[triple & 0x3F];
    }
    if (i < input.size()) {
        const bool two = i + 1 < input.size();
        const std::uint32_t triple =
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[i]) << 16 |
                                       (two ? static_cast<std::uint8_t>(input[i + 1]) << 8 : 0));
        out += kAlphabet[triple >> 18 & 0x3F];
        out += kAlphabet[triple >> 12 & 0x3F];
        out += two ? kAlphabet[triple >> 6 & 0x3F] : '=';
        out += '=';
    }
    return out;
}

}  // namespace krait::core::vt
