#pragma once

#include "core/parser/events.h"
#include "core/parser/tables.h"
#include "core/unicode/utf8.h"

#include <array>
#include <cstdint>
#include <span>

namespace krait::core::vt {

// Table-driven DEC ANSI parser (vt100.net/emu/dec_ansi_parser) with the
// deviations documented in tables.h. Allocation-free; hostile-input safe:
// params clamp at 16383 and cap at 32 parts, intermediates cap at 2 (overflow
// suppresses dispatch, xterm behavior), OSC/DCS payloads stream byte-wise to
// the events sink so the parser holds no unbounded buffer.
class Parser {
  public:
    // `acceptC1` enables 8-bit C1 controls (0x80-0x9F) as sequence
    // introducers/terminators. Off by default: C1 bytes collide with UTF-8
    // continuation bytes, so UTF-8 sessions must leave this off.
    explicit Parser(ParserEvents& events, bool acceptC1 = false) noexcept
        : m_events(events), m_acceptC1(acceptC1) {}

    void feed(std::span<const std::uint8_t> bytes);

  private:
    void feedByte(std::uint8_t byte);
    void handleC1(std::uint8_t byte);
    void transitionTo(State next, std::uint8_t byte);
    void doAction(Action action, std::uint8_t byte);
    void commitParam();
    void finishParams();
    void flushPendingUtf8();

    std::span<const std::uint8_t> intermediates() const noexcept {
        return {m_intermediates.data(), m_intermediateCount};
    }

    ParserEvents& m_events;
    Utf8Decoder m_decoder;
    Params m_params;
    std::array<std::uint8_t, 2> m_intermediates{};  // DEC max: 2
    std::uint8_t m_intermediateCount = 0;
    std::uint32_t m_paramValue = 0;
    bool m_paramPending = false;    // digits seen since last separator
    bool m_nextIsSub = false;       // next commit was preceded by ':'
    bool m_ignoreDispatch = false;  // intermediates overflowed: dispatch nothing
    bool m_dcsHooked = false;       // hook delivered, so put/unhook may flow
    State m_state = State::Ground;
    bool m_acceptC1;
};

}  // namespace krait::core::vt
