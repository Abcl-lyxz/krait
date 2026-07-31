#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace krait::core::vt {

class Grid;
struct Params;
class ReplyLimiter;

// The kitty keyboard protocol's negotiated flags (T48).
//
// https://sw.kovidgoyal.net/kitty/keyboard-protocol/
//
// The honesty rule (CLAUDE.md) has real teeth here, and the protocol is built
// for it: an application sets the flags it wants, then QUERIES to find out what
// it actually got. So the correct way to not support a flag is to mask it off
// on the way in and report the truth on the way out — never to store a bit we
// do not act on, because the query answer is a promise about the bytes the
// keyboard will send.
struct KittyKeyboard {
    // The five flags, in the protocol's bit order.
    static constexpr std::uint8_t kDisambiguate = 0b00001;
    static constexpr std::uint8_t kReportEvents = 0b00010;
    static constexpr std::uint8_t kReportAlternate = 0b00100;
    static constexpr std::uint8_t kReportAllAsEscapes = 0b01000;
    static constexpr std::uint8_t kReportText = 0b10000;

    // What the ENCODER actually does. M2 ships the baseline flag only; the
    // milestone's cut line puts full flags after it. Widening this constant
    // without widening translateKey() would make the query reply a lie.
    static constexpr std::uint8_t kSupported = kDisambiguate;

    // The protocol requires the stack to hold at least 16 entries, and to
    // discard rather than fail when it is full.
    static constexpr std::size_t kMaxDepth = 16;

    std::uint8_t flags = 0;

    // `CSI = flags ; mode u`. Mode 1 replaces, 2 sets the given bits, 3 clears
    // them. Anything else is ignored rather than guessed at.
    void apply(std::uint8_t requested, int mode) noexcept;
    // `CSI > flags u`
    void push(std::uint8_t requested) noexcept;
    // `CSI < count u`
    void pop(int count) noexcept;

    std::size_t depth() const noexcept { return m_depth; }

  private:
    std::array<std::uint8_t, kMaxDepth> m_stack{};
    std::size_t m_depth = 0;
};

// CSI ... u. Handles the four forms and appends the query reply to `out`.
// Returns false for a bare `CSI u` with no prefix, which belongs to nobody and
// stays honest silence.
bool handleKittyKeys(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                     std::uint8_t final, ReplyLimiter& limiter, std::string& out);

}  // namespace krait::core::vt
