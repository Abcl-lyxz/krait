#include "core/parser/csi_cursor.h"

#include <algorithm>

namespace krait::core::vt {

namespace {

// Wire parameter i: missing and 0 both mean the default, which is 1 for
// every sequence in this family (xterm ctlseqs).
int param(const Params& p, std::size_t i) noexcept {
    if (i >= p.count || p.values[i] == 0) {
        return 1;
    }
    return p.values[i];
}

}  // namespace

bool handleControl(Grid& grid, std::uint8_t control) noexcept {
    switch (control) {
    case 0x07:  // BEL
        ++grid.bells;
        return true;
    case 0x08:  // BS: one left, stops at column 1
        grid.col = std::max(0, grid.col - 1);
        grid.pendingWrap = false;
        return true;
    case 0x09:  // HT: fixed stops every 8 until HTS/TBC land
        grid.col = std::min(grid.cols - 1, (grid.col / 8 + 1) * 8);
        grid.pendingWrap = false;
        return true;
    case 0x0A:  // LF: down one, scrolling at the bottom
        grid.linefeed();
        return true;
    case 0x0D:  // CR
        grid.col = 0;
        grid.pendingWrap = false;
        return true;
    case 0x0E:  // SO: invoke G1
        grid.g1Invoked = true;
        return true;
    case 0x0F:  // SI: invoke G0
        grid.g1Invoked = false;
        return true;
    default:
        return false;
    }
}

bool handleCsiCursor(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                     std::uint8_t final) noexcept {
    if (!intermediates.empty()) {
        return false;
    }
    // xterm ignores CSI sequences carrying colon subparameters outside SGR.
    for (std::size_t i = 0; i < params.count; ++i) {
        if (params.subparam[i]) {
            return false;
        }
    }
    switch (final) {
    case 'A':  // CUU: stops at top (margins land with DECSTBM)
        grid.row = std::max(0, grid.row - param(params, 0));
        break;
    case 'B':  // CUD
        grid.row = std::min(grid.rows - 1, grid.row + param(params, 0));
        break;
    case 'C':  // CUF
        grid.col = std::min(grid.cols - 1, grid.col + param(params, 0));
        break;
    case 'D':  // CUB
        grid.col = std::max(0, grid.col - param(params, 0));
        break;
    case 'G':  // CHA: column absolute
        grid.col = std::clamp(param(params, 0) - 1, 0, grid.cols - 1);
        break;
    case 'd':  // VPA: row absolute
        grid.row = std::clamp(param(params, 0) - 1, 0, grid.rows - 1);
        break;
    case 'H':  // CUP
    case 'f':  // HVP (identical semantics)
        grid.row = std::clamp(param(params, 0) - 1, 0, grid.rows - 1);
        grid.col = std::clamp(param(params, 1) - 1, 0, grid.cols - 1);
        break;
    default:
        return false;
    }
    grid.pendingWrap = false;  // any explicit cursor motion cancels it
    return true;
}

}  // namespace krait::core::vt
