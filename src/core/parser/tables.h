#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace krait::core::vt {

// The 14 states of vt100.net/emu/dec_ansi_parser.
enum class State : std::uint8_t {
    Ground,
    Escape,
    EscapeIntermediate,
    CsiEntry,
    CsiParam,
    CsiIntermediate,
    CsiIgnore,
    DcsEntry,
    DcsParam,
    DcsIntermediate,
    DcsPassthrough,
    DcsIgnore,
    OscString,
    SosPmApcString,
    ApcString,
};

// The 14 actions of the same spec. None marks a pure transition; entry/exit
// actions (clear, hook/unhook, osc_start/osc_end) live in machine.cpp.
enum class Action : std::uint8_t {
    None,
    Ignore,
    Print,
    Execute,
    Clear,
    Collect,
    Param,
    EscDispatch,
    CsiDispatch,
    Hook,
    Put,
    Unhook,
    OscStart,
    OscPut,
    OscEnd,
    ApcStart,
    ApcPut,
    ApcEnd,
};

struct Entry {
    Action action;
    State next;  // == current state for "stay" entries
};

inline constexpr std::size_t kStateCount = 15;
using Row = std::array<Entry, 256>;

// Table generated at compile time from the spec's byte-range rules — the
// "generated tables" option of plan T5, without a codegen script.
//
// Documented deviations from the vt100.net diagram (docs/plan/02 T5):
//  - UTF-8 is decoded OUTSIDE the machine: Ground bytes >= 0x20 never reach
//    this table (machine.cpp feeds them to Utf8Decoder), and 0x80-0xFF inside
//    escape/CSI/DCS header states are ignored instead of being treated as GL
//    equivalents (X3.64 App. H would turn UTF-8 continuation bytes into
//    parameter digits — garbage injection on hostile input).
//  - 0x3A is a legal subparameter separator in CsiEntry/CsiParam (SGR 38:2::),
//    where the spec routes it to CsiIgnore. DCS keeps the spec routing.
//  - String states (OSC, DCS passthrough) pass 0x80-0xFF through as raw
//    payload so UTF-8 titles/payloads survive; 8-bit C1 (incl. 0x9C ST) is
//    intercepted before table lookup when the C1 policy flag is on.
//  - 0x07 BEL terminates OSC (universal xterm behavior; spec leaves OSC open).
//  - 0x7F DEL in Ground goes through the UTF-8 decoder and prints U+007F
//    (spec special-cases DEL as ignored); the display layer decides whether
//    to draw it. Inside sequences DEL is ignored per spec.
namespace detail {

constexpr void fill(Row& row, unsigned lo, unsigned hi, Action a, State next) {
    for (unsigned b = lo; b <= hi; ++b) {
        row[b] = {a, next};
    }
}

// C0 controls minus CAN(0x18), SUB(0x1A), ESC(0x1B) — those are "anywhere"
// keys applied last so they win in every state.
constexpr void fillC0(Row& row, Action a, State next) {
    fill(row, 0x00, 0x17, a, next);
    row[0x19] = {a, next};
    fill(row, 0x1C, 0x1F, a, next);
}

constexpr std::array<Row, kStateCount> build() {
    std::array<Row, kStateCount> t{};
    auto row = [&t](State s) -> Row& { return t[static_cast<std::size_t>(s)]; };

    // Default: ignore and stay. Covers DcsIgnore and SosPmApcString entirely,
    // plus 0x80-0xFF in every non-string state.
    for (std::size_t s = 0; s < kStateCount; ++s) {
        fill(t[s], 0x00, 0xFF, Action::Ignore, static_cast<State>(s));
    }

    {
        Row& r = row(State::Ground);
        fillC0(r, Action::Execute, State::Ground);
        // Fallback only: machine.cpp routes Ground >= 0x20 via the UTF-8
        // decoder, so these Print entries are never hit.
        fill(r, 0x20, 0x7F, Action::Print, State::Ground);
    }
    {
        Row& r = row(State::Escape);  // entry action: clear
        fillC0(r, Action::Execute, State::Escape);
        fill(r, 0x20, 0x2F, Action::Collect, State::EscapeIntermediate);
        fill(r, 0x30, 0x7E, Action::EscDispatch, State::Ground);
        r[0x50] = {Action::None, State::DcsEntry};
        r[0x58] = {Action::None, State::SosPmApcString};
        r[0x5B] = {Action::None, State::CsiEntry};
        r[0x5D] = {Action::None, State::OscString};
        r[0x5E] = {Action::None, State::SosPmApcString};
        // ESC _ is APC, which kitty's graphics protocol rides (T80). SOS
        // (0x58) and PM (0x5E) above stay in the ignore state: nothing
        // implements them, and giving them a payload would mean buffering
        // remote bytes for a protocol nobody speaks.
        r[0x5F] = {Action::None, State::ApcString};
        r[0x7F] = {Action::Ignore, State::Escape};
    }
    {
        Row& r = row(State::EscapeIntermediate);
        fillC0(r, Action::Execute, State::EscapeIntermediate);
        fill(r, 0x20, 0x2F, Action::Collect, State::EscapeIntermediate);
        fill(r, 0x30, 0x7E, Action::EscDispatch, State::Ground);
        r[0x7F] = {Action::Ignore, State::EscapeIntermediate};
    }
    {
        Row& r = row(State::CsiEntry);  // entry action: clear
        fillC0(r, Action::Execute, State::CsiEntry);
        fill(r, 0x20, 0x2F, Action::Collect, State::CsiIntermediate);
        fill(r, 0x30, 0x3B, Action::Param, State::CsiParam);  // 0x3A: deviation
        fill(r, 0x3C, 0x3F, Action::Collect, State::CsiParam);
        fill(r, 0x40, 0x7E, Action::CsiDispatch, State::Ground);
        r[0x7F] = {Action::Ignore, State::CsiEntry};
    }
    {
        Row& r = row(State::CsiParam);
        fillC0(r, Action::Execute, State::CsiParam);
        fill(r, 0x20, 0x2F, Action::Collect, State::CsiIntermediate);
        fill(r, 0x30, 0x3B, Action::Param, State::CsiParam);  // 0x3A: deviation
        fill(r, 0x3C, 0x3F, Action::None, State::CsiIgnore);
        fill(r, 0x40, 0x7E, Action::CsiDispatch, State::Ground);
        r[0x7F] = {Action::Ignore, State::CsiParam};
    }
    {
        Row& r = row(State::CsiIntermediate);
        fillC0(r, Action::Execute, State::CsiIntermediate);
        fill(r, 0x20, 0x2F, Action::Collect, State::CsiIntermediate);
        fill(r, 0x30, 0x3F, Action::None, State::CsiIgnore);
        fill(r, 0x40, 0x7E, Action::CsiDispatch, State::Ground);
        r[0x7F] = {Action::Ignore, State::CsiIntermediate};
    }
    {
        Row& r = row(State::CsiIgnore);
        fillC0(r, Action::Execute, State::CsiIgnore);
        fill(r, 0x40, 0x7E, Action::None, State::Ground);
    }
    {
        Row& r = row(State::DcsEntry);  // entry action: clear
        fillC0(r, Action::Ignore, State::DcsEntry);
        fill(r, 0x20, 0x2F, Action::Collect, State::DcsIntermediate);
        fill(r, 0x30, 0x39, Action::Param, State::DcsParam);
        r[0x3A] = {Action::None, State::DcsIgnore};
        r[0x3B] = {Action::Param, State::DcsParam};
        fill(r, 0x3C, 0x3F, Action::Collect, State::DcsParam);
        fill(r, 0x40, 0x7E, Action::None, State::DcsPassthrough);  // entry: hook
        r[0x7F] = {Action::Ignore, State::DcsEntry};
    }
    {
        Row& r = row(State::DcsParam);
        fillC0(r, Action::Ignore, State::DcsParam);
        fill(r, 0x20, 0x2F, Action::Collect, State::DcsIntermediate);
        fill(r, 0x30, 0x39, Action::Param, State::DcsParam);
        r[0x3A] = {Action::None, State::DcsIgnore};
        r[0x3B] = {Action::Param, State::DcsParam};
        fill(r, 0x3C, 0x3F, Action::None, State::DcsIgnore);
        fill(r, 0x40, 0x7E, Action::None, State::DcsPassthrough);
        r[0x7F] = {Action::Ignore, State::DcsParam};
    }
    {
        Row& r = row(State::DcsIntermediate);
        fillC0(r, Action::Ignore, State::DcsIntermediate);
        fill(r, 0x20, 0x2F, Action::Collect, State::DcsIntermediate);
        fill(r, 0x30, 0x3F, Action::None, State::DcsIgnore);
        fill(r, 0x40, 0x7E, Action::None, State::DcsPassthrough);
        r[0x7F] = {Action::Ignore, State::DcsIntermediate};
    }
    {
        Row& r = row(State::DcsPassthrough);  // entry: hook, exit: unhook
        fillC0(r, Action::Put, State::DcsPassthrough);
        fill(r, 0x20, 0x7E, Action::Put, State::DcsPassthrough);
        r[0x7F] = {Action::Ignore, State::DcsPassthrough};
        fill(r, 0x80, 0xFF, Action::Put, State::DcsPassthrough);  // raw UTF-8
    }
    {
        Row& r = row(State::ApcString);           // entry: apc_start, exit: apc_end
        r[0x07] = {Action::None, State::Ground};  // same BEL deviation as OSC
        fill(r, 0x20, 0x7F, Action::ApcPut, State::ApcString);
        fill(r, 0x80, 0xFF, Action::ApcPut, State::ApcString);  // raw base64/UTF-8
    }
    {
        Row& r = row(State::OscString);           // entry: osc_start, exit: osc_end
        r[0x07] = {Action::None, State::Ground};  // deviation: BEL terminator
        fill(r, 0x20, 0x7F, Action::OscPut, State::OscString);
        fill(r, 0x80, 0xFF, Action::OscPut, State::OscString);  // raw UTF-8
    }

    // "Anywhere" keys, applied last so they override every state row.
    for (std::size_t s = 0; s < kStateCount; ++s) {
        t[s][0x18] = {Action::Execute, State::Ground};  // CAN
        t[s][0x1A] = {Action::Execute, State::Ground};  // SUB
        t[s][0x1B] = {Action::None, State::Escape};     // ESC (entry: clear)
    }
    return t;
}

}  // namespace detail

inline constexpr std::array<Row, kStateCount> kTable = detail::build();

}  // namespace krait::core::vt
