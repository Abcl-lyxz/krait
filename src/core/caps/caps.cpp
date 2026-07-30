#include "core/caps/caps.h"

#include <algorithm>
#include <format>
#include <string>

namespace krait::core::vt {

std::string da1Reply(const Capabilities& caps) {
    // VT220-level identity only when a VT220-level feature is truthfully on.
    std::string ext;
    const auto add = [&ext](bool on, int code) {
        if (on) {
            ext += ';';
            ext += std::to_string(code);
        }
    };
    add(caps.columns132, 1);
    add(caps.printerPort, 2);
    add(caps.regis, 3);
    add(caps.sixel, 4);
    add(caps.selectiveErase, 6);
    add(caps.ansiColor, 22);
    if (ext.empty()) {
        return caps.avo ? "\x1B[?1;2c" : "\x1B[?1;0c";
    }
    return "\x1B[?62" + ext + "c";
}

bool handleReport(const Grid& grid, const Capabilities& caps, const Params& params,
                  std::span<const std::uint8_t> intermediates, std::uint8_t final,
                  ReplyLimiter& limiter, std::string& out) {
    if (!intermediates.empty()) {
        return false;  // DA2 (>), DECXCPR/DEC DSR (?), etc.: not implemented
    }
    for (std::size_t i = 0; i < params.count; ++i) {
        if (params.subparam[i]) {
            return false;  // colon subparams are SGR-only
        }
    }
    switch (final) {
    case 'c':  // DA1: only CSI c / CSI 0 c is ours (xterm ignores 0;1 forms)
        if (params.count > 1 || (params.count == 1 && params.values[0] != 0)) {
            return false;
        }
        if (limiter.allow()) {
            out += da1Reply(caps);
        }
        return true;
    case 'n': {  // DSR
        const int ps = params.count > 0 ? params.values[0] : 0;
        if (ps == 5) {  // operating status: OK
            if (limiter.allow()) {
                out += "\x1B[0n";
            }
            return true;
        }
        if (ps == 6) {  // CPR, 1-based
            // With DECOM set the position an application asked for was
            // margin-relative, so the one it is told back must be too — a CPR
            // that ignored origin mode would break the save/restore round trip
            // full-screen apps do (vt100.net DECOM; xterm reports
            // `cur_row - top_marg` when ORIGIN is on).
            // Clamped at 0: a mode-1049 restore can legitimately leave the
            // cursor ABOVE the top margin with DECOM still set, and an
            // unclamped subtraction would emit `ESC [ -18 ; 1 R`. '-' is not a
            // CSI parameter byte (ECMA-48 5.4), so the application would drop
            // the whole reply and any app blocking on CPR would hang. xterm
            // gets away with it via unsigned ParmType; we clamp, as xterm does
            // in the adjacent DSR branch.
            const int reportRow =
                grid.originMode ? std::max(0, grid.row - grid.scrollTop) : grid.row;
            if (limiter.allow()) {
                out += std::format("\x1B[{};{}R", reportRow + 1, grid.col + 1);
            }
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

ModeReport decrqmState(const Grid& grid, const Capabilities& caps, std::uint16_t mode) noexcept {
    switch (mode) {
    case 6:  // DECOM, origin mode
        return grid.originMode ? ModeReport::Set : ModeReport::Reset;

    case 7:   // DECAWM, autowrap
    case 25:  // DECTCEM, cursor visibility
        // Both are on with no code path that turns them off: putChar's
        // deferred wrap is unconditional, and nothing hides the cursor until
        // the real renderer (T25). So the honest answer is PERMANENTLY set,
        // not "set" — answering 1 would promise an application it can turn
        // them off, and it would then misdraw wondering why that did not take.
        // Making either a real toggle is grid/renderer work, not a reply
        // change, and this line moves when that lands.
        //
        // Shared branch on purpose: bugprone-branch-clone gates the build and
        // two identical returns would trip it, exactly as mode 2027 did in T19.
        return ModeReport::PermanentlySet;

    case 1049:
        return grid.onAlternateScreen() ? ModeReport::Set : ModeReport::Reset;

    case 1:  // DECCKM, application cursor keys
        return grid.appCursorKeys ? ModeReport::Set : ModeReport::Reset;

    // The three mouse tracking modes share one variable (grid.h), so each
    // answers Set only when it is the ACTIVE one. Reporting 1000 as set while
    // 1003 is active would be a lie an application acts on.
    case 1000:
        return grid.mouseTracking == Grid::MouseTracking::Normal ? ModeReport::Set
                                                                 : ModeReport::Reset;
    case 1002:
        return grid.mouseTracking == Grid::MouseTracking::ButtonEvent ? ModeReport::Set
                                                                      : ModeReport::Reset;
    case 1003:
        return grid.mouseTracking == Grid::MouseTracking::AnyEvent ? ModeReport::Set
                                                                   : ModeReport::Reset;

    case 1006:  // mouse: SGR encoding
        return grid.sgrMouse ? ModeReport::Set : ModeReport::Reset;

    case 2004:
        return grid.bracketedPaste ? ModeReport::Set : ModeReport::Reset;

    case 2026:
        // What the application asked for, not what the guard is doing — see
        // SyncOutput::requested().
        return grid.sync.requested() ? ModeReport::Set : ModeReport::Reset;

    case 2027:
        // Grapheme clustering. Always on by construction (T19/ADR-0003), so 3
        // and never 1. This is the reply Capabilities::graphemeClusteringAlwaysOn
        // exists to generate.
        return caps.graphemeClusteringAlwaysOn ? ModeReport::PermanentlySet
                                               : ModeReport::NotRecognized;

    default:
        // Not recognised. Every mode we merely CONSUME without implementing
        // must land here: claiming 2 ("reset, you may set it") for a mode we
        // would then ignore is the same lie as claiming 1.
        return ModeReport::NotRecognized;
    }
}

bool handleDecrqm(const Grid& grid, const Capabilities& caps, const Params& params,
                  std::span<const std::uint8_t> intermediates, std::uint8_t final,
                  ReplyLimiter& limiter, std::string& out) {
    if (final != 'p' || intermediates.size() != 2 || intermediates[0] != '?' ||
        intermediates[1] != '$') {
        return false;
    }
    // DECRQM asks about exactly ONE mode: unlike DECSET it takes no list, and
    // a multi-parameter form is malformed rather than a batch request.
    if (params.count != 1 || params.subparam[0]) {
        return false;
    }
    if (!limiter.allow()) {
        return true;  // rate-dropped, but handled
    }
    const auto mode = static_cast<std::uint16_t>(params.values[0]);
    out += "[?";
    out += std::to_string(mode);
    out += ';';
    out += std::to_string(static_cast<int>(decrqmState(grid, caps, mode)));
    out += "$y";
    return true;
}

}  // namespace krait::core::vt
