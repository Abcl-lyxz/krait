#include "core/parser/csi_mode.h"

#include <string>

namespace krait::core::vt {

namespace {

// Mode 2048's report: CSI 48 ; rows ; cols ; height_px ; width_px t.
//
// Rows and columns come FIRST, then pixels, and both pairs are
// height-before-width — the opposite order to how a window size is usually
// written. Getting it backwards produces a report every consumer accepts and
// every consumer misreads, so the order is spelled out here rather than left
// to the reader of the format string.
//
// The pixel figures are the TEXT AREA, not the window: the spec excludes any
// padding the terminal applies, which is why they are computed from the cell
// size rather than taken from the widget.
std::string resizeReport(const Grid& grid) {
    const int rows = grid.rows;
    const int cols = grid.cols;
    return "\x1b[48;" + std::to_string(rows) + ";" + std::to_string(cols) + ";" +
           std::to_string(rows * grid.cellHeightPx) + ";" +
           std::to_string(cols * grid.cellWidthPx) + "t";
}

void applyMode(Grid& grid, std::uint16_t mode, bool on, std::string* reply) {
    switch (mode) {
    case 6:  // DECOM
        grid.originMode = on;
        // BOTH set and reset home the cursor: xterm's DECOM case is
        // `(*func)(&term->flags, ORIGIN); CursorSet(screen, 0, 0, term->flags)`
        // — the CursorSet is unconditional and reads the flags it just changed.
        // So set homes to the top margin, reset homes to absolute row 0.
        grid.cursorSet(0, 0);
        break;

    case 1:
        // DECCKM. Only the input path reads it; nothing on screen moves.
        grid.appCursorKeys = on;
        break;

    case 1000:
    case 1002:
    case 1003: {
        // xterm keeps these in ONE variable, so setting a higher mode replaces
        // the lower one, and resetting a mode that is not the active one is a
        // no-op. With three independent flags, `1003l` after `1000h 1003h`
        // leaves normal tracking on and the application — which believes it
        // disabled the mouse — starts receiving reports as keyboard input.
        const auto wanted = mode == 1000   ? Grid::MouseTracking::Normal
                            : mode == 1002 ? Grid::MouseTracking::ButtonEvent
                                           : Grid::MouseTracking::AnyEvent;
        if (on) {
            grid.mouseTracking = wanted;
        } else if (grid.mouseTracking == wanted) {
            grid.mouseTracking = Grid::MouseTracking::Off;
        }
        break;
    }

    case 1006:
        // SGR mouse encoding. Independent of WHETHER we track: an application
        // may enable it before or after the tracking mode.
        grid.sgrMouse = on;
        break;

    case 2004:
        // Bracketed paste. A flag and nothing more at this layer — see grid.h.
        grid.bracketedPaste = on;
        break;

    case 2026:
        // Synchronized output. DECSET opens a batch, DECRST closes it. The
        // timestamp is 0 here because src/core/ has no clock: the app layer
        // re-stamps the batch as it feeds bytes in, and the guard is evaluated
        // against ITS clock. A core-only test drives begin() directly.
        if (on) {
            grid.sync.begin(grid.nowMs);
        } else {
            grid.sync.end();
        }
        break;

    case 2031:
        // Colour-palette update notification (T83). A switch and nothing more:
        // the notification says whether the theme reads light or dark, and
        // src/core/ has no theme — the app raises it. Turning it ON sends
        // nothing, unlike 2048 below: the sequence for "tell me now" is a
        // separate DSR this does not answer, so inventing a reply here would be
        // a sequence no application is listening for.
        grid.paletteUpdates = on;
        break;

    case 2048:
        // In-band resize notification (T82). Enabling it MUST report the
        // current size immediately — the spec says so in as many words, and it
        // is the whole point: an application enables this to stop guessing at
        // the size, and making it wait for the next resize means guessing once
        // more. So unlike every other mode here this one has a reply, which is
        // why applyMode gained an out-parameter.
        grid.inBandResize = on;
        if (on && reply != nullptr) {
            *reply = resizeReport(grid);
        }
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
        // T22's DECRQM answers 3 (permanently set) for it, never 1, generated
        // from Capabilities::graphemeClusteringAlwaysOn.
        break;
    }
}

}  // namespace

bool handleMode(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                std::uint8_t final, std::string* reply) {
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
        applyMode(grid, params.values[i], on, reply);
    }
    return true;
}

}  // namespace krait::core::vt
