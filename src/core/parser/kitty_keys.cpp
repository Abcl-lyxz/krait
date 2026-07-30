#include "core/parser/kitty_keys.h"

#include "core/caps/caps.h"
#include "core/grid/grid.h"
#include "core/parser/events.h"

namespace krait::core::vt {

namespace {

std::uint16_t paramAt(const Params& params, std::size_t index, std::uint16_t fallback) {
    if (index >= params.count || params.values[index] == 0) {
        return fallback;
    }
    return params.values[index];
}

// The one place a requested flag set becomes a stored one. Everything the
// encoder does not implement is dropped here, so the query reply below cannot
// promise behavior that does not exist.
std::uint8_t honest(std::uint16_t requested) noexcept {
    return static_cast<std::uint8_t>(requested) & KittyKeyboard::kSupported;
}

}  // namespace

void KittyKeyboard::apply(std::uint8_t requested, int mode) noexcept {
    switch (mode) {
    case 1:
        flags = requested;
        break;
    case 2:
        flags = static_cast<std::uint8_t>(flags | requested);
        break;
    case 3:
        flags = static_cast<std::uint8_t>(flags & ~requested);
        break;
    default:
        // An unknown mode is not "probably replace". Doing nothing leaves the
        // application's own query able to tell it that nothing happened.
        break;
    }
}

void KittyKeyboard::push(std::uint8_t requested) noexcept {
    if (m_depth == kMaxDepth) {
        // The protocol says to discard the OLDEST entry rather than refuse, so
        // a program that pushes without popping degrades instead of wedging.
        for (std::size_t i = 1; i < kMaxDepth; ++i) {
            m_stack[i - 1] = m_stack[i];
        }
        --m_depth;
    }
    m_stack[m_depth++] = flags;
    flags = requested;
}

void KittyKeyboard::pop(int count) noexcept {
    // Popping past the bottom leaves the protocol OFF rather than leaving
    // whatever happened to be underneath: a program that lost track of its own
    // stack should land in the state that sends legacy keys, which is the one
    // every application can read.
    for (int i = 0; i < count; ++i) {
        flags = m_depth > 0 ? m_stack[--m_depth] : 0;
    }
}

bool handleKittyKeys(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                     std::uint8_t final, ReplyLimiter& limiter, std::string& out) {
    if (final != 'u' || intermediates.size() != 1) {
        return false;
    }
    KittyKeyboard& keys = grid.kittyKeys;

    switch (intermediates[0]) {
    case '?': {
        // The negotiation. Report what is actually ON, which after the masking
        // in every setter is only ever something the encoder implements.
        if (!limiter.allow()) {
            return true;  // rate-dropped, but still ours and still handled
        }
        out += "\x1b[?";
        out += std::to_string(static_cast<unsigned>(keys.flags));
        out += 'u';
        return true;
    }
    case '>':
        keys.push(honest(paramAt(params, 0, 0)));
        return true;
    case '<':
        keys.pop(paramAt(params, 0, 1));
        return true;
    case '=':
        keys.apply(honest(paramAt(params, 0, 0)), paramAt(params, 1, 1));
        return true;
    default:
        // CSI, some other private marker, and 'u' is not ours.
        return false;
    }
}

}  // namespace krait::core::vt
