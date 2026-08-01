#pragma once

#include "core/grid/line.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace krait::core::vt {

// OSC payload accumulation and dispatch (T49).
//
// The parser streams OSC bytes one at a time and holds no buffer of its own
// (machine.h), so somebody has to. That somebody is here, and it is the first
// place in the core that accumulates unbounded remote input — which makes the
// cap the load-bearing part, not the parsing.

// What an OSC string turned out to be. The app layer acts on these; the core
// never touches a clipboard or opens a URL.
struct OscAction {
    enum class Kind : std::uint8_t {
        None,
        // OSC 8: a hyperlink starts (uri non-empty) or ends (uri empty).
        Hyperlink,
        // OSC 52: the application wants to WRITE the clipboard.
        ClipboardWrite,
        // OSC 52 with `?`: it wants to READ it. Never answered without
        // explicit per-session permission (rules/net.md).
        ClipboardRead,
        // OSC 0/2: window title.
        Title,
        // OSC 133: a semantic shell-integration mark. Handled inside the core
        // FIRST — the mark belongs on the grid line, not in the app layer —
        // and then forwarded to onOsc as well, because the C -> D transition
        // is also the only signal the app has that a command finished and how
        // long it took (T67's long-command notification).
        PromptMark,
        // OSC 9;4: taskbar progress. Nothing in the core acts on it — a
        // taskbar is a platform surface — so this is purely a report.
        Progress,
    };

    // OSC 9 ; 4 ; <state> ; <progress>. ConEmu originated the sequence and
    // Microsoft documents it for Windows Terminal; the two agree on 0-3 and
    // DISAGREE on 4, which MS calls "Warning" and ConEmu calls "paused". Both
    // render as the same yellow bar, so the name here follows the Win32 flag
    // they both end up setting (TBPF_PAUSED) rather than picking a side.
    enum class Progress : std::uint8_t {
        Remove = 0,         // 0: no progress bar. Also what an ABSENT state means.
        Set = 1,            // 1: determinate, at `percent`
        Error = 2,          // 2: red bar; percent is optional (ConEmu)
        Indeterminate = 3,  // 3: marquee; consumers IGNORE percent (MS says so)
        Paused = 4,         // 4: MS "Warning", ConEmu "paused"; percent optional
    };

    Kind kind = Kind::None;
    // Hyperlink target, decoded clipboard bytes, or title text depending on
    // `kind`. Always already length-checked.
    std::string text;
    // OSC 52's selection characters ("c", "p", "s"...). Kept as sent, so the
    // app can refuse the ones it does not implement rather than guess.
    std::string selection;
    // OSC 8's id= parameter, which is what lets two runs of cells be the SAME
    // link for hover purposes even when they are not adjacent.
    std::string id;
    // OSC 133: exactly one of the kMark* bits from line.h.
    std::uint8_t promptMark = 0;
    // OSC 133 ; D ; <n> only. -1 when the shell sent a bare D, or sent a
    // status this parser refused.
    int exitCode = -1;
    // OSC 9;4 only.
    Progress progress = Progress::Remove;
    // OSC 9;4's percentage, already clamped to 0-100. -1 means the string did
    // not carry a readable one, which states 2 and 4 are explicitly allowed to
    // do; kept distinct from 0 so the app can tell "no figure given" from
    // "zero" and pick a default per state rather than showing an empty bar for
    // a failure. Parsed for EVERY state, including 0 and 3 which do not use it
    // — refusing to read a field is not the core's call to make, and the app
    // decides what a state does with it.
    int percent = -1;
};

class OscHandler {
  public:
    // Remote input, so bounded before it is anything else. 8 KiB holds any sane
    // URI and a comfortable clipboard line; a terminal that buffers megabytes
    // because a server said OSC is a terminal a server can stop.
    static constexpr std::size_t kMaxPayload = 8 * 1024;
    // net.md asks for OSC 52 size caps by name. Base64 shrinks on decode, so
    // this bounds the decoded side independently of the payload cap.
    static constexpr std::size_t kMaxClipboardBytes = 4 * 1024;

    void start() noexcept;
    void put(std::uint8_t byte);
    // Returns what the completed string asked for. An aborted string yields
    // None: xterm discards those and so do we.
    OscAction end(bool aborted);

    // True when the payload hit the cap. The dispatch is then refused outright
    // rather than acted on in part — half a URI is a different URI.
    bool overflowed() const noexcept { return m_overflowed; }

  private:
    std::string m_buffer;
    bool m_overflowed = false;
};

// Base64 for OSC 52. Decoding returns false on anything that is not valid
// base64 rather than decoding as far as it can: a partially decoded clipboard
// is worse than a refused one.
bool decodeBase64(std::string_view input, std::string* out, std::size_t maxBytes);
std::string encodeBase64(std::string_view input);

}  // namespace krait::core::vt
