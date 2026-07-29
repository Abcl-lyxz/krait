#include "core/parser/csi_scroll.h"

namespace krait::core::vt {

namespace {

// Ps with a default of 1, and 0 meaning 1 — the convention for every counted
// CSI parameter in this parser (see csi_cursor).
int countParam(const Params& p, std::size_t i) noexcept {
    if (i >= p.count || p.values[i] == 0) {
        return 1;
    }
    return static_cast<int>(p.values[i]);
}

}  // namespace

bool handleScroll(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                  std::uint8_t final) noexcept {
    if (!intermediates.empty()) {
        return false;  // CSI ? r, CSI > T etc. belong to other families
    }
    for (std::size_t i = 0; i < params.count; ++i) {
        if (params.subparam[i]) {
            return false;  // subparameters are SGR-only
        }
    }

    switch (final) {
    case 'r': {  // DECSTBM
        // xterm clamps the parameters FIRST and only then rejects an inverted
        // region: Pt < 1 becomes 1, and an omitted/zero/oversize Pb becomes the
        // page height. So `CSI 0;99 r` on a 24-row screen is a valid full-screen
        // region, not an error.
        int top = params.count > 0 ? static_cast<int>(params.values[0]) : 0;
        int bottom = params.count > 1 ? static_cast<int>(params.values[1]) : 0;
        if (top < 1) {
            top = 1;
        }
        if (bottom < 1 || bottom > grid.rows) {
            bottom = grid.rows;
        }
        // Inverted or degenerate region: xterm's `if (bot > top)` has no else,
        // so the whole sequence is ignored — margins keep their old values AND
        // the cursor does not move. DEC STD 070 is stricter still (it also
        // ignores an oversize bottom); xterm's clamp wins for app compatibility.
        if (bottom <= top) {
            return true;
        }
        grid.scrollTop = top - 1;
        grid.scrollBottom = bottom - 1;
        // "DECSTBM moves the cursor to column 1, line 1 of the page" — which is
        // the region's top line when origin mode is on.
        grid.row = grid.originMode ? grid.scrollTop : 0;
        grid.col = 0;
        grid.pendingWrap = false;
        return true;
    }

    case 'L':    // IL
    case 'M': {  // DL
        // Both are no-ops outside the region (DEC: "IL has no effect outside
        // the page margins"). Left/right confinement would need DECSLRM, which
        // we never enable, so full-width is correct here.
        if (!grid.inScrollRegion(grid.row)) {
            return true;
        }
        const int n = countParam(params, 0);
        // Insert/delete inside the region is a scroll of the sub-region running
        // from the cursor row to the bottom margin — that is what makes lines
        // below the cursor move while lines above it stay put.
        const int savedTop = grid.scrollTop;
        grid.scrollTop = grid.row;
        if (final == 'L') {
            grid.scrollRegionDown(n);
        } else {
            grid.scrollRegionUp(n);
        }
        grid.scrollTop = savedTop;
        // xterm's InsertLine/DeleteLine both end with
        // `set_cur_col(screen, ScrnLeftMargin(xw))`. Note konsole, urxvt and
        // the linux console do NOT move the column; we follow xterm/VTE.
        grid.col = 0;
        grid.pendingWrap = false;
        return true;
    }

    case 'S':  // SU
        grid.scrollRegionUp(countParam(params, 0));
        // Unlike IL/DL there is no in-region gate and no cursor movement:
        // xtermScroll() reads only the margins and never touches the cursor.
        return true;

    case 'T':  // SD, if it really is SD
        // SD is "distinguished by having only one parameter and having the
        // first parameter != 0". Everything else with this final is a mouse
        // tracking form (or an undocumented arity); consume and ignore rather
        // than guess.
        if (params.count == 1 && params.values[0] != 0) {
            grid.scrollRegionDown(static_cast<int>(params.values[0]));
        }
        return true;

    default:
        return false;
    }
}

}  // namespace krait::core::vt
