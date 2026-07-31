#pragma once

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
