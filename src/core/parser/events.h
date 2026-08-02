#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace krait::core::vt {

// Numeric parameters of a CSI or DCS sequence. `subparam[i]` is true when
// values[i] was separated from values[i-1] by ':' (SGR 38:2:r:g:b style).
// An empty part is stored as 0; consumers map 0 to the sequence's default.
struct Params {
    static constexpr std::size_t kMaxParams = 32;      // plan deviation: cap 32
    static constexpr std::uint16_t kMaxValue = 16383;  // DEC 14-bit, clamped

    std::array<std::uint16_t, kMaxParams> values{};
    std::array<bool, kMaxParams> subparam{};
    std::size_t count = 0;
};

// Sink for the parser's dispatch actions. `intermediates` spans hold private
// markers (0x3C-0x3F) and intermediate bytes (0x20-0x2F) in arrival order;
// the spans are only valid for the duration of the call.
class ParserEvents {
  public:
    virtual ~ParserEvents() = default;

    virtual void print(char32_t cp) = 0;
    virtual void execute(std::uint8_t control) = 0;
    virtual void escDispatch(std::span<const std::uint8_t> intermediates, std::uint8_t final) = 0;
    virtual void csiDispatch(const Params& params, std::span<const std::uint8_t> intermediates,
                             std::uint8_t final) = 0;
    virtual void dcsHook(const Params& params, std::span<const std::uint8_t> intermediates,
                         std::uint8_t final) = 0;
    // The parser enforces no length cap on OSC/DCS payloads (it holds no
    // buffer); sinks that accumulate put() bytes must bound their own growth.
    virtual void dcsPut(std::uint8_t byte) = 0;
    // `aborted` is true when the string was cut short by CAN/SUB or a C1
    // introducer instead of a terminator; xterm discards aborted strings and
    // sinks should too. ESC is assumed to begin ST and counts as clean.
    virtual void dcsUnhook(bool aborted) = 0;
    virtual void oscStart() = 0;
    virtual void oscPut(std::uint8_t byte) = 0;
    virtual void oscEnd(bool aborted) = 0;

    // APC (ESC _ ... ST), which the kitty graphics protocol rides (T80).
    //
    // These have DEFAULT no-op bodies while everything above is pure, and the
    // asymmetry is deliberate: APC was ignored outright until M5, so a sink
    // that goes on ignoring it behaves exactly as it did before. Making them
    // pure would force three empty overrides into every existing sink — the
    // corpus harness, the fuzz target — purely to express "unchanged".
    virtual void apcStart() {}

    virtual void apcPut(std::uint8_t) {}

    virtual void apcEnd(bool /*aborted*/) {}
};

}  // namespace krait::core::vt
