#include "core/parser/csi_mode.h"

namespace krait::core::vt {

namespace {

void applyMode(Grid& grid, std::uint16_t mode, bool on) noexcept {
    switch (mode) {
    case 6:  // DECOM
        grid.originMode = on;
        // BOTH set and reset home the cursor: xterm's DECOM case is
        // `(*func)(&term->flags, ORIGIN); CursorSet(screen, 0, 0, term->flags)`
        // — the CursorSet is unconditional and reads the flags it just changed.
        // So set homes to the top margin, reset homes to absolute row 0.
        grid.cursorSet(0, 0);
        break;

    case 1049:
        // NOT idempotent, and deliberately so. xterm's whichBuf guard lives
        // INSIDE ToAlternate/FromAlternate — CursorSave, ClearScreen and
        // CursorRestore around them run unconditionally (charproc.c
        // srm_OPT_ALTBUF_CURSOR). So a repeated 1049h really does re-clear the
        // alternate screen, and a cold 1049l really does restore a
        // never-written slot, homing the cursor and resetting the pen.
        if (on) {
            // ctlseqs: "Save cursor as in DECSC ... After saving the cursor,
            // switch to the Alternate Screen Buffer, clearing it first." The
            // save happens BEFORE the switch, so it lands in the normal
            // screen's slot and clobbers a pending ESC 7 — as xterm does.
            grid.saveCursor();
            grid.useAlternateScreen(true);
            grid.eraseScreen();
            // xterm drops the pending-wrap state on activation WITHOUT wrapping
            // first, so a cursor parked on the last column does not eat a row.
            grid.pendingWrap = false;
        } else {
            // "Use Normal Screen Buffer and restore cursor as in DECRC." No
            // clear on the way out — clearing on reset is 1047's behavior.
            grid.useAlternateScreen(false);
            grid.restoreCursor();
        }
        break;

    default:
        // Consumed and ignored. Nothing may report an unrecognised mode as
        // recognised, which is why this handler keeps an explicit list above
        // rather than a catch-all setter.
        //
        // Mode 2027 (grapheme clustering) lands here on purpose and needs no
        // case of its own: our width model is always cluster-based (T19), so
        // there is no per-codepoint mode to switch to and both DECSET and
        // DECRST are correctly inert. It is NOT merely unrecognised though —
        // DECRQM (T22) must answer 3 (permanently set) for it, never 1, and the
        // fact lives in Capabilities::graphemeClusteringAlwaysOn where that
        // reply will be generated from.
        break;
    }
}

}  // namespace

bool handleMode(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                std::uint8_t final) noexcept {
    if (final != 'h' && final != 'l') {
        return false;
    }
    // Exactly one private marker, and it must be '?'.
    if (intermediates.size() != 1 || intermediates[0] != '?') {
        return false;
    }
    for (std::size_t i = 0; i < params.count; ++i) {
        if (params.subparam[i]) {
            return false;  // subparameters are SGR-only
        }
    }
    // DECSET takes a LIST — `CSI ? 6 ; 1049 h` sets both, in order. A parameter
    // of 0 (or `CSI ? h` with no parameters at all) names no mode.
    const bool on = final == 'h';
    for (std::size_t i = 0; i < params.count; ++i) {
        applyMode(grid, params.values[i], on);
    }
    return true;
}

}  // namespace krait::core::vt
