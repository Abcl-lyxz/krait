#include "core/parser/machine.h"

#include <algorithm>

namespace krait::core::vt {

namespace {

constexpr Action entryAction(State s) noexcept {
    switch (s) {
    case State::Escape:
    case State::CsiEntry:
    case State::DcsEntry:
        return Action::Clear;
    case State::OscString:
        return Action::OscStart;
    case State::DcsPassthrough:
        return Action::Hook;
    default:
        return Action::None;
    }
}

constexpr Action exitAction(State s) noexcept {
    switch (s) {
    case State::OscString:
        return Action::OscEnd;
    case State::DcsPassthrough:
        return Action::Unhook;
    default:
        return Action::None;
    }
}

}  // namespace

void Parser::feed(std::span<const std::uint8_t> bytes) {
    for (std::uint8_t b : bytes) {
        feedByte(b);
    }
}

void Parser::feedByte(std::uint8_t byte) {
    if (m_acceptC1 && byte >= 0x80 && byte <= 0x9F) {
        handleC1(byte);
        return;
    }
    if (m_state == State::Ground) {
        if (byte >= 0x20) {
            // Deviation: UTF-8 lives outside the machine. All Ground text
            // (ASCII included) goes through the WHATWG decoder, which is a
            // pass-through for well-formed input and emits U+FFFD for damage.
            char32_t out[2];
            const int n = m_decoder.feed(byte, out);
            for (int i = 0; i < n; ++i) {
                m_events.print(out[i]);
            }
            return;
        }
        // A C0 control interrupts any pending UTF-8 sequence.
        flushPendingUtf8();
    }
    const Entry entry = kTable[static_cast<std::size_t>(m_state)][byte];
    if (entry.next == m_state) {
        doAction(entry.action, byte);
        return;
    }
    // Spec ordering: exit action, then transition action, then entry action.
    doAction(exitAction(m_state), byte);
    doAction(entry.action, byte);
    m_state = entry.next;
    doAction(entryAction(entry.next), byte);
}

// 8-bit C1 "anywhere" transitions (only reached when acceptC1 is on).
void Parser::handleC1(std::uint8_t byte) {
    if (m_state == State::Ground) {
        flushPendingUtf8();
    }
    switch (byte) {
    case 0x90:
        transitionTo(State::DcsEntry, byte);
        break;
    case 0x9B:
        transitionTo(State::CsiEntry, byte);
        break;
    case 0x9D:
        transitionTo(State::OscString, byte);
        break;
    case 0x98:
    case 0x9E:
    case 0x9F:
        transitionTo(State::SosPmApcString, byte);
        break;
    case 0x9C:  // ST: terminates strings via the exit action, else no-op
        transitionTo(State::Ground, byte);
        break;
    default:
        transitionTo(State::Ground, byte);
        m_events.execute(byte);
        break;
    }
}

// Unlike the table path, runs entry actions even for same-state transitions
// so e.g. 0x9B inside a CSI restarts it with a clear.
void Parser::transitionTo(State next, std::uint8_t byte) {
    doAction(exitAction(m_state), byte);
    m_state = next;
    doAction(entryAction(next), byte);
}

void Parser::doAction(Action action, std::uint8_t byte) {
    switch (action) {
    case Action::None:
    case Action::Ignore:
        break;
    case Action::Print:  // unreachable fallback; Ground text is decoded in feedByte
        m_events.print(byte);
        break;
    case Action::Execute:
        m_events.execute(byte);
        break;
    case Action::Clear:
        m_params = {};
        m_paramValue = 0;
        m_paramPending = false;
        m_nextIsSub = false;
        m_intermediateCount = 0;
        m_ignoreDispatch = false;
        m_dcsHooked = false;
        break;
    case Action::Collect:
        if (m_intermediateCount < m_intermediates.size()) {
            m_intermediates[m_intermediateCount++] = byte;
        } else {
            m_ignoreDispatch = true;  // xterm: overlong sequence dispatches nothing
        }
        break;
    case Action::Param:
        if (byte == ';' || byte == ':') {
            commitParam();
            m_nextIsSub = (byte == ':');
        } else {
            m_paramValue =
                std::min<std::uint32_t>(m_paramValue * 10 + (byte - '0'), Params::kMaxValue);
            m_paramPending = true;
        }
        break;
    case Action::EscDispatch:
        if (!m_ignoreDispatch) {
            m_events.escDispatch(intermediates(), byte);
        }
        break;
    case Action::CsiDispatch:
        finishParams();
        if (!m_ignoreDispatch) {
            m_events.csiDispatch(m_params, intermediates(), byte);
        }
        break;
    case Action::Hook:
        finishParams();
        if (!m_ignoreDispatch) {
            m_events.dcsHook(m_params, intermediates(), byte);
            m_dcsHooked = true;
        }
        break;
    case Action::Put:
        if (m_dcsHooked) {
            m_events.dcsPut(byte);
        }
        break;
    case Action::Unhook:
        // Aborted unless ended by ST (ESC assumed to begin ESC \).
        if (m_dcsHooked) {
            m_events.dcsUnhook(byte != 0x1B && byte != 0x9C);
            m_dcsHooked = false;
        }
        break;
    case Action::OscStart:
        m_events.oscStart();
        break;
    case Action::OscPut:
        m_events.oscPut(byte);
        break;
    case Action::OscEnd:
        m_events.oscEnd(byte != 0x07 && byte != 0x1B && byte != 0x9C);
        break;
    }
}

// Commits the part accumulated since the last separator. A separator with no
// digits commits an empty part as 0 (consumers treat 0 as "default").
void Parser::commitParam() {
    if (m_params.count < Params::kMaxParams) {
        m_params.values[m_params.count] = static_cast<std::uint16_t>(m_paramValue);
        m_params.subparam[m_params.count] = m_nextIsSub;
        ++m_params.count;
    }
    // ponytail: parts past kMaxParams are dropped silently (plan cap 32);
    // the sequence still dispatches with the first 32.
    m_paramValue = 0;
    m_paramPending = false;
}

// Commits the trailing part at dispatch. "CSI H" has no params at all, but
// "CSI 1;H" has a trailing empty part — any earlier separator implies one.
void Parser::finishParams() {
    if (m_paramPending || m_params.count > 0) {
        commitParam();
    }
}

void Parser::flushPendingUtf8() {
    char32_t tail[1];
    if (m_decoder.finish(tail) == 1) {
        m_events.print(tail[0]);
    }
}

}  // namespace krait::core::vt
