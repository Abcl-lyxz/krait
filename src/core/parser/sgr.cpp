#include "core/parser/sgr.h"

#include <algorithm>

namespace krait::core::vt {

namespace {

// Number of extra LEGACY (semicolon) parameters an extended-color
// introducer consumes: 38;5;Pi -> 2, 38;2;Pr;Pg;Pb -> 4, malformed -> 0.
std::size_t legacyColorArity(const Params& p, std::size_t i) noexcept {
    if (i + 1 >= p.count) {
        return 0;
    }
    if (p.values[i + 1] == 5) {
        return 2;
    }
    if (p.values[i + 1] == 2) {
        return 4;
    }
    return 0;
}

}  // namespace

bool applySgr(Attr& pen, const Params& params,
              std::span<const std::uint8_t> intermediates) noexcept {
    if (!intermediates.empty()) {
        return false;  // CSI > m, CSI ? m etc. are other sequences
    }
    if (params.count == 0) {
        pen = {};  // bare CSI m == SGR 0
        return true;
    }
    std::size_t i = 0;
    while (i < params.count) {
        // Subparameters following a base parameter belong to it.
        std::size_t next = i + 1;
        while (next < params.count && params.subparam[next]) {
            ++next;
        }
        const bool hasSub = next > i + 1;
        switch (params.values[i]) {
        case 0:
            pen = {};
            break;
        case 1:
            pen.flags |= Attr::kBold;
            break;
        case 2:
            pen.flags |= Attr::kDim;
            break;
        case 3:
            pen.flags |= Attr::kItalic;
            break;
        case 4:
            // 4:0 = off; 4:1..n styles collapse to plain underline until M1.
            if (hasSub && params.values[i + 1] == 0) {
                pen.flags &= static_cast<std::uint16_t>(~Attr::kUnderline);
            } else {
                pen.flags |= Attr::kUnderline;
            }
            break;
        case 5:
            pen.flags |= Attr::kBlink;
            break;
        case 7:
            pen.flags |= Attr::kReverse;
            break;
        case 8:
            pen.flags |= Attr::kInvisible;
            break;
        case 9:
            pen.flags |= Attr::kStrike;
            break;
        case 21:  // doubly underlined: approximated as underline until M1
            pen.flags |= Attr::kUnderline;
            break;
        case 22:
            pen.flags &= static_cast<std::uint16_t>(~(Attr::kBold | Attr::kDim));
            break;
        case 23:
            pen.flags &= static_cast<std::uint16_t>(~Attr::kItalic);
            break;
        case 24:
            pen.flags &= static_cast<std::uint16_t>(~Attr::kUnderline);
            break;
        case 25:
            pen.flags &= static_cast<std::uint16_t>(~Attr::kBlink);
            break;
        case 27:
            pen.flags &= static_cast<std::uint16_t>(~Attr::kReverse);
            break;
        case 28:
            pen.flags &= static_cast<std::uint16_t>(~Attr::kInvisible);
            break;
        case 29:
            pen.flags &= static_cast<std::uint16_t>(~Attr::kStrike);
            break;
        case 39:
            pen.fg = {};
            break;
        case 49:
            pen.bg = {};
            break;
        case 38:
        case 48:
        case 58:
            // Extended color skeleton (M1): consume args, change nothing.
            if (!hasSub) {
                next = std::min(next + legacyColorArity(params, i), params.count);
            }
            break;
        case 59:  // underline color reset (M1): tolerated no-op
            break;
        default: {
            const int v = params.values[i];
            if (v >= 30 && v <= 37) {
                pen.fg = {Color::Kind::Indexed, static_cast<std::uint8_t>(v - 30), 0};
            } else if (v >= 40 && v <= 47) {
                pen.bg = {Color::Kind::Indexed, static_cast<std::uint8_t>(v - 40), 0};
            } else if (v >= 90 && v <= 97) {
                pen.fg = {Color::Kind::Indexed, static_cast<std::uint8_t>(v - 90 + 8), 0};
            } else if (v >= 100 && v <= 107) {
                pen.bg = {Color::Kind::Indexed, static_cast<std::uint8_t>(v - 100 + 8), 0};
            }
            // Anything else: unknown SGR, ignored.
            break;
        }
        }
        i = next;
    }
    return true;
}

bool handleErase(StubGrid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
                 std::uint8_t final) noexcept {
    if (!intermediates.empty()) {
        return false;
    }
    // Colon subparams are SGR-only (same rejection as the cursor family).
    for (std::size_t i = 0; i < params.count; ++i) {
        if (params.subparam[i]) {
            return false;
        }
    }
    const int mode = params.count > 0 ? params.values[0] : 0;
    const auto clearRange = [&grid](int fromRow, int fromCol, int toRow, int toCol) {
        for (int r = fromRow; r <= toRow; ++r) {
            const int c0 = (r == fromRow) ? fromCol : 0;
            const int c1 = (r == toRow) ? toCol : grid.cols - 1;
            for (int c = c0; c <= c1; ++c) {
                grid.cells[static_cast<std::size_t>(r) * grid.cols + c] = Cell{};
            }
        }
    };
    switch (final) {
    case 'J':  // ED
        if (mode == 0) {
            clearRange(grid.row, grid.col, grid.rows - 1, grid.cols - 1);
        } else if (mode == 1) {
            clearRange(0, 0, grid.row, grid.col);  // inclusive of cursor
        } else if (mode == 2) {
            clearRange(0, 0, grid.rows - 1, grid.cols - 1);
        }
        // mode 3 (scrollback): no-op until the real grid; others ignored
        return true;
    case 'K':  // EL
        if (mode == 0) {
            clearRange(grid.row, grid.col, grid.row, grid.cols - 1);
        } else if (mode == 1) {
            clearRange(grid.row, 0, grid.row, grid.col);
        } else if (mode == 2) {
            clearRange(grid.row, 0, grid.row, grid.cols - 1);
        }
        return true;
    default:
        return false;
    }
}

}  // namespace krait::core::vt
