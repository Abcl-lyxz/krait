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

// An OSC 133 ; D exit status. Digits only, overflow-checked, and refused
// whole rather than clamped: a status is a number a user will read as "this
// failed with 137", and a truncated one is a different answer, not a rounded
// one. A leading '-' is refused for the same reason — every POSIX shell sends
// `$?`, which is 0-255, and inventing a sign convention here would make Krait
// disagree with whatever the shell meant.
bool parseExitStatus(std::string_view text, int* out) {
    if (text.empty() || text.size() > 10) {
        return false;
    }
    long long value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + (ch - '0');
        if (value > 0x7FFFFFFFLL) {
            return false;
        }
    }
    *out = static_cast<int>(value);
    return true;
}

// True when a `k=` option says this prompt is NOT the one a user navigates to.
// kitty: "Just before starting to draw the PS2 prompt send ...;A;k=s". ghostty
// reads the same alphabet — i initial, r right, c continuation, s secondary.
//
// This is the difference between jump-to-prompt landing on a command and
// landing in the middle of one: without it every PS2 continuation row of a
// multi-line command is a prompt start, and worse, `D`'s exit status attaches
// to the LAST continuation row instead of the line the command began on.
//
// FAIL-OPEN on a kind letter we do not know. The spec rule is the same one OSC
// 8's parameters follow — ignore unrecognised options — and ghostty maps an
// unknown kind to null, i.e. unspecified, i.e. still a prompt. Testing
// `!= "k=i"` instead was fail-CLOSED: `k=`, `k=x` and `k=initial` each dropped
// the mark entirely, so the day a shell starts emitting a kind letter this
// alphabet does not have, the user loses EVERY jump target rather than one
// distinction. Only the three known non-navigable kinds suppress the mark.
bool isSecondaryPrompt(std::string_view params) {
    while (!params.empty()) {
        std::string_view tail;
        const std::string_view field = upTo(params, ';', &tail);
        if (field.starts_with("k=")) {
            const std::string_view kind = field.substr(2);
            return kind == "s" || kind == "r" || kind == "c";
        }
        params = tail;
    }
    return false;  // no k= at all means the primary prompt
}

// OSC 133 ; <command> [; <key>=<value> ...] ST, and for D
// OSC 133 ; D [; <exit status>] [; <key>=<value> ...] ST.
//
// Unrecognised `key=value` options — `aid=`, `cl=`, `click_events=1`,
// `cmdline_url=` — are SKIPPED, never fatal: the list grows with every terminal
// that adds an extension, and losing a prompt boundary the user can see because
// a shell learned a new key is the worse failure. `k=` is NOT in that category
// and is read above; it changes what the command means.
OscAction parseShellIntegration(std::string_view rest) {
    std::string_view params;
    const std::string_view command = upTo(rest, ';', &params);

    OscAction action;
    if (command == "A" || command == "P") {
        // BOTH, and this is load-bearing rather than generous. ghostty names
        // them `fresh_line_new_prompt` and `prompt_start`, and `P;k=i` is what
        // wezterm's shipped integration, DomTerm's, and zsh's own
        // Src/Zle/termquery.c actually emit — handling only `A` would place
        // ZERO marks for three real shell integrations.
        if (isSecondaryPrompt(params)) {
            return {};  // PS2 / RPROMPT / continuation: not a jump target
        }
        action.promptMark = kMarkPromptStart;
    } else if (command == "B") {
        action.promptMark = kMarkInputStart;
    } else if (command == "C") {
        action.promptMark = kMarkOutputStart;
    } else if (command == "D") {
        action.promptMark = kMarkCommandEnd;
        // POSITIONAL, first field only — what ghostty does
        // (`parseInt(i32, full, 10) catch null`) and what wezterm emits
        // (`\033]133;D;%s;aid=%s\007`, status first). Scanning later fields for
        // "something that looks like a number" would make `D;oops;5` report 5
        // where every other terminal reports nothing.
        std::string_view tail;
        parseExitStatus(upTo(params, ';', &tail), &action.exitCode);
    } else {
        // L (fresh line), N (new command) and I (end prompt, terminate at EOL)
        // are spec'd letters we have not implemented; anything else is noise.
        // Silence rather than a guessed mark — see docs/conformance.md.
        return {};
    }
    action.kind = OscAction::Kind::PromptMark;
    return action;
}

// OSC 9 ; 4 ; <state> [; <progress>] ST — taskbar progress.
//
// Spec, verified rather than remembered: Microsoft documents the sequence for
// Windows Terminal (learn.microsoft.com "Set the progress bar in Windows
// Terminal") and ConEmu, which originated it, documents it as
// `ESC ] 9 ; 4 ; st ; pr ST`. Both terminate on ST or BEL, which the Williams
// machine already handles for every OSC.
//
// Remote input, so every field is refused rather than guessed at:
//   * the subcommand must be exactly "4". OSC 9 with anything else is ConEmu's
//     notification / iTerm2's growl, which Krait does not implement — silence
//     is the honest answer, not a taskbar poke.
//   * the state must be a single digit 0-4. "01", "10", "4x" and "" all mean
//     the sender is confused, and picking the nearest legal state would make
//     the taskbar say something nobody asked for.
//   * the percentage is CLAMPED to 0-100 (MS: "a number between 0 and 100,
//     inclusive"); neither source says what an out-of-range value does, so
//     clamping is our choice and is documented in docs/conformance.md. A
//     non-numeric or absent percentage is simply absent, because states 2 and
//     4 are allowed to omit it.
OscAction parseProgress(std::string_view rest) {
    std::string_view params;
    if (upTo(rest, ';', &params) != "4") {
        return {};
    }

    OscAction action;
    action.kind = OscAction::Kind::Progress;

    std::string_view tail;
    const std::string_view state = upTo(params, ';', &tail);
    if (state.empty()) {
        // `OSC 9;4 ST` and `OSC 9;4; ST` mean REMOVE, not "malformed". That is
        // what Windows Terminal's own dispatcher does (AdaptDispatch's ConEmu
        // action reads a missing or empty state as 0), and refusing it would be
        // the worse failure of the two available: a bar left asserting progress
        // for a command that has finished, with nothing able to clear it.
        return action;
    }
    if (state.size() != 1 || state[0] < '0' || state[0] > '4') {
        return {};
    }
    action.progress = static_cast<OscAction::Progress>(state[0] - '0');

    // Positional first field, like OSC 133 ; D's status: a later field that
    // happens to look like a number is an option, not the percentage.
    //
    // parseExitStatus is the same digits-only, overflow-checked, sign-refusing
    // reader OSC 133 ; D uses — reused rather than copied so "what counts as a
    // number in an OSC parameter" has exactly one answer in this file.
    int percent = 0;
    if (parseExitStatus(upTo(tail, ';', &tail), &percent)) {
        action.percent = percent > 100 ? 100 : percent;
    }
    return action;
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

    if (code == "133") {
        return parseShellIntegration(rest);
    }

    if (code == "9") {
        return parseProgress(rest);
    }

    // Everything else is honest silence. OSC 4 (palette), 10/11 (fg/bg) and 7
    // (cwd) are later milestones, and acting on them now would claim behavior
    // that does not exist.
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
