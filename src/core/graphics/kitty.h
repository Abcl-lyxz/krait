#pragma once

#include "core/graphics/image.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace krait::core::vt {

// The kitty graphics protocol (M5 T80).
//
// Wire form: ESC _ G <control data> ; <payload> ESC \
// Control data is comma-separated key=value pairs; the payload is base64.
//
// Verified against sw.kovidgoyal.net/kitty/graphics-protocol rather than
// remembered. What this implements is DIRECT transmission of uncompressed RGB
// and RGBA — what `kitty icat` and every sending library fall back to against a
// terminal whose capabilities they do not know — plus placement and deletion.
//
// What it does NOT implement, and each is a REFUSAL rather than a silent drop
// (see Command::error), because a sender that is told can fall back and a
// sender that is ignored just prints nothing:
//   f=100  PNG. Decoding PNG in src/core/ means a decoder dependency in the
//          zero-dependency zone (rules/vt-core.md). Declined, and the sender
//          falls back to RGB/RGBA, which the protocol is designed for.
//   o=z    zlib. Same argument.
//   t=f/t/s  transmission by file, temp file or shared memory. A remote host
//          naming a LOCAL path is this protocol's sharpest edge: over SSH the
//          path is on the wrong machine, and honouring it would let a server
//          read this machine's files by asking the terminal to do it. Direct
//          transmission only.
//   a=f/a  animation.
struct Command {
    enum class Action : std::uint8_t {
        Transmit,        // a=t
        TransmitAndPut,  // a=T (the default)
        Put,             // a=p
        Delete,          // a=d
        Query,           // a=q
        Unsupported,     // a=f, a=a, or anything unknown
    };

    enum class Format : std::uint8_t {
        Rgb = 24,
        Rgba = 32,
        Png = 100,
    };

    Action action = Action::TransmitAndPut;
    Format format = Format::Rgba;
    std::uint32_t id = 0;           // i=
    std::uint32_t number = 0;       // I=
    std::uint32_t placementId = 0;  // p=
    int width = 0;                  // s=
    int height = 0;                 // v=
    int srcX = 0;                   // x=
    int srcY = 0;                   // y=
    int srcW = 0;                   // w=
    int srcH = 0;                   // h=
    int cols = 0;                   // c=
    int rows = 0;                   // r=
    int zIndex = 0;                 // z=
    bool moreChunks = false;        // m=1
    bool cursorStays = false;       // C=1
    // q=1 suppresses the OK, q=2 suppresses errors too. A terminal that
    // answered anyway would corrupt the output of a program that asked for
    // silence precisely because it is not reading replies.
    int quiet = 0;
    // Non-empty when the command names something this terminal declines.
    std::string error;

    // The decoded pixels, once the last chunk has arrived. Empty for a command
    // that carried no data (a=p, a=d) or that is still assembling.
    Image image;
    bool complete = false;
};

// Accumulates one APC string, and across the chunked transmissions kitty splits
// a large image into (m=1 ... m=0).
class KittyDecoder {
  public:
    // Payload bound. Kitty caps a CHUNK at 4096 base64 bytes; this bounds the
    // ASSEMBLED total, which is the number that matters — a sender can send
    // unlimited chunks.
    static constexpr std::size_t kMaxPayloadBytes = 24u * 1024 * 1024;

    void start() noexcept;
    void put(std::uint8_t byte);

    // Nullopt when the string was not a graphics command at all (an APC that
    // does not begin with 'G' belongs to somebody else), or was aborted, or is
    // a chunk with more to come.
    std::optional<Command> end(bool aborted);

  private:
    std::optional<Command> finish();

    std::string m_buffer;
    // Carried across chunks: only the FIRST escape of a chunked transmission
    // carries the full parameters, so the rest have to inherit them.
    Command m_pending;
    std::string m_pendingData;
    bool m_assembling = false;
    bool m_overflowed = false;
};

// Parses the control half — the part before the ';'. Exposed because it is
// pure, total, and the half most worth testing directly.
Command parseControl(std::string_view control);

// The reply a command earns, or empty for silence. Terminals answer ONLY when
// the sender supplied i= or I=, because the reply is addressed to those and a
// sender without one is not listening for it.
std::string kittyReply(const Command& command);

}  // namespace krait::core::vt
