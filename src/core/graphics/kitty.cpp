#include "core/graphics/kitty.h"

#include "core/parser/osc.h"

#include <algorithm>
#include <charconv>
#include <system_error>
#include <utility>

namespace krait::core::vt {
namespace {

// A key's value as a signed integer. Total: anything unparseable leaves the
// field at its default rather than aborting the command, which is what the spec
// asks for ("keys the terminal does not understand must be ignored") and is the
// only reading that survives the protocol gaining a key.
long long integerOf(std::string_view text, long long fallback) {
    long long value = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    // The WHOLE field must be a number. Accepting "12x" as 12 is how a z-index
    // ends up meaning something the sender did not write.
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return fallback;
    }
    return value;
}

int clampToInt(long long value, int lowest, int highest) {
    return static_cast<int>(std::clamp<long long>(value, lowest, highest));
}

std::uint32_t clampToId(long long value) {
    return value <= 0 || value > 0xFFFFFFFFLL ? 0U : static_cast<std::uint32_t>(value);
}

}  // namespace

Command parseControl(std::string_view control) {
    Command command;
    // Every key is optional, and an unknown one is skipped rather than fatal.
    while (!control.empty()) {
        const std::size_t comma = control.find(',');
        const std::string_view pair = control.substr(0, comma);
        control = comma == std::string_view::npos ? std::string_view{} : control.substr(comma + 1);
        const std::size_t equals = pair.find('=');
        if (equals != 1) {
            continue;  // keys are single characters
        }
        const char key = pair[0];
        const std::string_view value = pair.substr(equals + 1);
        if (value.empty()) {
            continue;
        }

        switch (key) {
        case 'a':
            switch (value[0]) {
            case 't':
                command.action = Command::Action::Transmit;
                break;
            case 'T':
                command.action = Command::Action::TransmitAndPut;
                break;
            case 'p':
                command.action = Command::Action::Put;
                break;
            case 'd':
                command.action = Command::Action::Delete;
                break;
            case 'q':
                command.action = Command::Action::Query;
                break;
            default:
                command.action = Command::Action::Unsupported;
                break;
            }
            break;
        case 'f': {
            const long long format = integerOf(value, 32);
            if (format == 24) {
                command.format = Command::Format::Rgb;
            } else if (format == 32) {
                command.format = Command::Format::Rgba;
            } else if (format == 100) {
                command.format = Command::Format::Png;
                // Declined rather than dropped. A sender that is TOLD falls
                // back to RGB/RGBA — which is what the protocol's format
                // negotiation is for — and one that is ignored prints nothing
                // and gives no reason.
                command.error = "EINVAL:PNG is not supported; send f=24 or f=32";
            } else {
                command.error = "EINVAL:unknown format";
            }
            break;
        }
        case 't':
            if (value[0] != 'd') {
                // The sharpest edge in this protocol. `t=f` names a path on the
                // machine running the TERMINAL, so over SSH a remote host could
                // ask this terminal to read a local file and draw it — a
                // file-disclosure primitive pointed at the user. rules/net.md:
                // remote input is hostile. Refused rather than sandboxed,
                // because there is no version of "read the file a remote host
                // named" that is safe here.
                command.error = "EINVAL:only direct transmission (t=d) is supported";
            }
            break;
        case 'o':
            // Compression. Same argument as PNG: a decompressor in the
            // zero-dependency zone, for a fallback the sender already has.
            command.error = "EINVAL:compression is not supported";
            break;
        case 's':
            command.width = clampToInt(integerOf(value, 0), 0, kMaxImageDimension);
            break;
        case 'v':
            command.height = clampToInt(integerOf(value, 0), 0, kMaxImageDimension);
            break;
        case 'i':
            command.id = clampToId(integerOf(value, 0));
            break;
        case 'I':
            command.number = clampToId(integerOf(value, 0));
            break;
        case 'p':
            command.placementId = clampToId(integerOf(value, 0));
            break;
        case 'x':
            command.srcX = clampToInt(integerOf(value, 0), 0, kMaxImageDimension);
            break;
        case 'y':
            command.srcY = clampToInt(integerOf(value, 0), 0, kMaxImageDimension);
            break;
        case 'w':
            command.srcW = clampToInt(integerOf(value, 0), 0, kMaxImageDimension);
            break;
        case 'h':
            command.srcH = clampToInt(integerOf(value, 0), 0, kMaxImageDimension);
            break;
        case 'c':
            command.cols = clampToInt(integerOf(value, 0), 0, 10000);
            break;
        case 'r':
            command.rows = clampToInt(integerOf(value, 0), 0, 10000);
            break;
        case 'z':
            // Signed, and the sign is the feature: a negative z draws UNDER the
            // text, which is what a background watermark is.
            command.zIndex = clampToInt(integerOf(value, 0), -1000000, 1000000);
            break;
        case 'm':
            command.moreChunks = integerOf(value, 0) == 1;
            break;
        case 'C':
            command.cursorStays = integerOf(value, 0) == 1;
            break;
        case 'q':
            command.quiet = clampToInt(integerOf(value, 0), 0, 2);
            break;
        default:
            break;  // an unknown key is skipped, as the spec requires
        }
    }
    if (command.action == Command::Action::Unsupported && command.error.empty()) {
        command.error = "EINVAL:unsupported action";
    }
    return command;
}

std::string kittyReply(const Command& command) {
    // Answered ONLY when the sender supplied i= or I=. The reply is addressed
    // to those, and a sender without one is not reading — writing anyway would
    // scatter `_Gi=0;OK` through the output of every program that sends an
    // image without asking for confirmation.
    if (command.id == 0 && command.number == 0) {
        return {};
    }
    const bool failed = !command.error.empty();
    // q=1 suppresses the OK, q=2 suppresses errors as well. Honoured exactly,
    // because a program that asked for silence is one whose output a stray
    // reply would corrupt.
    if ((!failed && command.quiet >= 1) || (failed && command.quiet >= 2)) {
        return {};
    }

    std::string reply = "\x1b_G";
    if (command.id != 0) {
        reply += "i=" + std::to_string(command.id);
    }
    if (command.number != 0) {
        if (command.id != 0) {
            reply += ",";
        }
        reply += "I=" + std::to_string(command.number);
    }
    reply += ";";
    reply += failed ? command.error : "OK";
    reply += "\x1b\\";
    return reply;
}

void KittyDecoder::start() noexcept {
    m_buffer.clear();
    m_overflowed = false;
}

void KittyDecoder::put(std::uint8_t byte) {
    // The control half is tiny; the payload is what needs bounding, and the
    // ASSEMBLED total is bounded in finish(). This cap is the per-escape one,
    // generous enough for kitty's 4096-byte chunk and for a sender that ignores
    // the chunk guidance and puts a whole small image in one string.
    constexpr std::size_t kMaxOneString = std::size_t{1024} * 1024;
    if (m_buffer.size() >= kMaxOneString) {
        m_overflowed = true;
        return;
    }
    m_buffer.push_back(static_cast<char>(byte));
}

std::optional<Command> KittyDecoder::end(bool aborted) {
    if (aborted || m_overflowed) {
        // An aborted chunk discards the WHOLE assembly, not just itself: the
        // remaining chunks would be spliced onto a truncated prefix and decode
        // to noise that still passes every size check.
        m_assembling = false;
        m_pendingData.clear();
        m_buffer.clear();
        m_overflowed = false;
        return std::nullopt;
    }
    std::optional<Command> result = finish();
    m_buffer.clear();
    return result;
}

std::optional<Command> KittyDecoder::finish() {
    // An APC that does not begin with 'G' is somebody else's — iTerm2's file
    // protocol rides APC too — and must not be answered or acted on.
    if (m_buffer.empty() || m_buffer.front() != 'G') {
        return std::nullopt;
    }
    const std::string_view body = std::string_view(m_buffer).substr(1);
    const std::size_t semicolon = body.find(';');
    const std::string_view control = body.substr(0, semicolon);
    const std::string_view payload =
        semicolon == std::string_view::npos ? std::string_view{} : body.substr(semicolon + 1);

    Command command = parseControl(control);
    if (m_assembling) {
        // A continuation carries only m= and maybe q=. Everything else — the
        // format, the size, the id — belongs to the first escape, so the
        // pending command is what the payload is appended to.
        const bool more = command.moreChunks;
        command = m_pending;
        command.moreChunks = more;
    }

    if (!payload.empty()) {
        std::string decoded;
        // Bounded on the DECODED side. Base64 shrinks by a quarter, so a cap on
        // the encoded form would let a sender exceed the real budget.
        const std::size_t room =
            kMaxPayloadBytes > m_pendingData.size() ? kMaxPayloadBytes - m_pendingData.size() : 0;
        if (!decodeBase64(payload, &decoded, room)) {
            m_assembling = false;
            m_pendingData.clear();
            command.error = "EINVAL:payload is not valid base64, or is too large";
            command.complete = true;
            return command;
        }
        m_pendingData += decoded;
    }

    if (command.moreChunks) {
        m_pending = command;
        m_pending.error.clear();  // errors are reported once, at the end
        m_assembling = true;
        return std::nullopt;  // nothing to act on until the last chunk
    }

    m_assembling = false;
    std::string data = std::exchange(m_pendingData, {});
    command.complete = true;

    // A command carrying no pixels (a=p places an image already here, a=d
    // deletes one, a=q asks) is complete as it stands.
    if (data.empty() || command.action == Command::Action::Put ||
        command.action == Command::Action::Delete || command.action == Command::Action::Query) {
        return command;
    }
    if (!command.error.empty()) {
        return command;  // declined format or medium; the pixels are unusable
    }

    const int bytesPerPixel = command.format == Command::Format::Rgb ? 3 : 4;
    if (command.width <= 0 || command.height <= 0) {
        command.error = "EINVAL:s and v are required for raw pixel data";
        return command;
    }
    const auto pixelCount =
        static_cast<std::size_t>(command.width) * static_cast<std::size_t>(command.height);
    if (pixelCount > kMaxImagePixels) {
        command.error = "EINVAL:image is too large";
        return command;
    }
    if (data.size() < pixelCount * static_cast<std::size_t>(bytesPerPixel)) {
        // Refused rather than padded. A short payload padded with black is an
        // image the sender did not send, drawn over the user's terminal.
        command.error = "EINVAL:payload is shorter than s x v says";
        return command;
    }

    command.image.width = command.width;
    command.image.height = command.height;
    command.image.pixels.resize(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t at = i * static_cast<std::size_t>(bytesPerPixel);
        const auto r = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at]));
        const auto g = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + 1]));
        const auto b = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + 2]));
        // RGB has no alpha channel, so it is opaque. Defaulting it to zero
        // would decode a correct image and then draw nothing at all.
        const std::uint32_t a =
            bytesPerPixel == 4 ? static_cast<std::uint8_t>(data[at + 3]) : 0xFFU;
        command.image.pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    return command;
}

}  // namespace krait::core::vt
