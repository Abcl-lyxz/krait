#include "core/parser/sgr.h"

#include <algorithm>

namespace krait::core::vt {

namespace {

// Outcome of reading one 38/48/58 argument list.
struct ExtColor {
    bool ok = false;           // false: malformed/out of range -> change nothing
    Color color;               //
    std::size_t consumed = 1;  // parameters used, INCLUDING the introducer
};

constexpr bool isByte(std::uint16_t v) noexcept {
    return v <= 255;
}

Color rgbOf(std::uint16_t r, std::uint16_t g, std::uint16_t b) noexcept {
    return {Color::Kind::Rgb, 0,
            (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) |
                static_cast<std::uint32_t>(b)};
}

// 38/48/58 arrive in three shapes, and telling them apart is the whole job:
//
//   legacy semicolons     38;5;Ps           38;2;Pr;Pg;Pb        no color-space id
//   colon, ITU/xterm      38:5:Ps           38:2:Pi:Pr:Pg:Pb     Pi present, ignored
//   colon, Pi omitted     -                 38:2:Pr:Pg:Pb        common in the wild
//
// The two colon RGB forms are distinguished by COUNTING subparameters, not by
// guessing which one an application meant: 5 OR MORE after the introducer
// means Pi is there, exactly 4 means it is not. It must stay ">= 5", never
// "== 5" — the ITU form may carry trailing tolerance parameters
// (38:2:Pi:R:G:B:Ps:Pt), which xterm documents as "ignores parameters 6 (and
// above)". An empty Pi (38:2::R:G:B) parses to 0 and lands in the 5-subparam
// case on its own, so it needs no special path.
//
// `subEnd` is one past the last subparameter of the introducer at `i`.
ExtColor readExtendedColor(const Params& p, std::size_t i, std::size_t subEnd) noexcept {
    ExtColor out;
    const std::size_t subCount = subEnd - i - 1;

    if (subCount > 0) {
        // Colon form: every subparameter belongs to this introducer, so all of
        // them are consumed even when the payload turns out to be unusable.
        out.consumed = subEnd - i;
        const std::uint16_t kind = p.values[i + 1];
        if (kind == 5 && subCount >= 2) {
            if (!isByte(p.values[i + 2])) {
                return out;
            }
            out.color = {Color::Kind::Indexed, static_cast<std::uint8_t>(p.values[i + 2]), 0};
            out.ok = true;
        } else if (kind == 2 && subCount >= 4) {
            const std::size_t r = i + (subCount >= 5 ? 3 : 2);
            if (!isByte(p.values[r]) || !isByte(p.values[r + 1]) || !isByte(p.values[r + 2])) {
                return out;
            }
            out.color = rgbOf(p.values[r], p.values[r + 1], p.values[r + 2]);
            out.ok = true;
        }
        return out;
    }

    // Legacy form: plain parameters. Consume only what is actually present so
    // a truncated `38;5` cannot swallow whatever follows it.
    const std::size_t avail = p.count - i - 1;
    if (avail == 0) {
        return out;
    }
    const std::uint16_t kind = p.values[i + 1];

    // xterm also accepts the MIXED spelling, where the introducer is separated
    // by a semicolon but the kind carries its own colon run — its source names
    // both: "accept CSI 38 ; 5 : 1 m" and "accept CSI 38 ; 2 : 1 : 2 : 3 m".
    // The Pi rule shifts accordingly: xterm uses `have > 3` here, counting the
    // subparameters hanging off the kind rather than off the introducer.
    if (i + 2 < p.count && p.subparam[i + 2]) {
        std::size_t runEnd = i + 2;
        while (runEnd < p.count && p.subparam[runEnd]) {
            ++runEnd;
        }
        const std::size_t have = runEnd - (i + 2);
        out.consumed = runEnd - i;
        if (kind == 5 && have >= 1 && isByte(p.values[i + 2])) {
            out.color = {Color::Kind::Indexed, static_cast<std::uint8_t>(p.values[i + 2]), 0};
            out.ok = true;
        } else if (kind == 2 && have >= 3) {
            const std::size_t r = i + 2 + (have > 3 ? 1 : 0);
            if (isByte(p.values[r]) && isByte(p.values[r + 1]) && isByte(p.values[r + 2])) {
                out.color = rgbOf(p.values[r], p.values[r + 1], p.values[r + 2]);
                out.ok = true;
            }
        }
        return out;
    }

    if (kind == 5) {
        out.consumed = std::min<std::size_t>(3, avail + 1);
        if (avail >= 2 && isByte(p.values[i + 2])) {
            out.color = {Color::Kind::Indexed, static_cast<std::uint8_t>(p.values[i + 2]), 0};
            out.ok = true;
        }
        return out;
    }
    if (kind == 2) {
        out.consumed = std::min<std::size_t>(5, avail + 1);
        if (avail >= 4 && isByte(p.values[i + 2]) && isByte(p.values[i + 3]) &&
            isByte(p.values[i + 4])) {
            out.color = rgbOf(p.values[i + 2], p.values[i + 3], p.values[i + 4]);
            out.ok = true;
        }
        return out;
    }
    // Unknown color kind: consume the introducer AND the kind. xterm does the
    // same (extended_colors_limit() yields 0, and its loop then steps past the
    // kind). Consuming only the introducer would leave e.g. the 7 of
    // `38;7;1;2` to execute as SGR 7 — hostile input synthesising reverse
    // video, which no reference terminal produces.
    out.consumed = 2;
    return out;
}

// SGR 4:n. kitty's prose documents 0-5 and is silent above that, but its
// implementation clamps — `decoration = MIN(5, params[i])` (kitty cursor.c) —
// so 4:9 renders dashed, not single. Match the implementation: Underline has
// exactly those six values, so the clamp maps 1:1.
constexpr Underline underlineFromSub(std::uint16_t v) noexcept {
    return static_cast<Underline>(v > 5 ? 5 : v);
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
        // Only 4 and the colour introducers take subparameters. xterm ignores
        // a subparameter group on any other code (`item += skip; op = 9999`)
        // and kitty rejects the whole sequence; acting on the base would be
        // worse than either. It also disarms a leading colon: `CSI :3 m`
        // parses its empty first part as 0, which would otherwise fire SGR 0
        // and wipe every attribute — the loudest possible response to
        // malformed input, and one no reference terminal gives.
        const std::uint16_t code = params.values[i];
        if (hasSub && code != 4 && code != 38 && code != 48 && code != 58) {
            i = next;
            continue;
        }
        switch (code) {
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
            // Bare `4` is single; `4:n` selects a style (kitty 4:0-4:5).
            pen.underline = hasSub ? underlineFromSub(params.values[i + 1]) : Underline::Single;
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
        case 21:  // "Doubly-underlined, ECMA-48 3rd" per xterm ctlseqs.
            pen.underline = Underline::Double;
            break;
        case 22:
            pen.flags &= static_cast<std::uint16_t>(~(Attr::kBold | Attr::kDim));
            break;
        case 23:
            pen.flags &= static_cast<std::uint16_t>(~Attr::kItalic);
            break;
        case 24:
            pen.underline = Underline::None;
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
        case 58: {
            const ExtColor ext = readExtendedColor(params, i, next);
            if (ext.ok) {
                Color& target = params.values[i] == 38   ? pen.fg
                                : params.values[i] == 48 ? pen.bg
                                                         : pen.ul;
                target = ext.color;
            }
            // Consume the arguments even when unusable, so a malformed color
            // never turns its own payload into the SGRs that follow it.
            next = std::min(i + ext.consumed, params.count);
            // And never let `next` land ON a subparameter: the top of the loop
            // guarantees `i` addresses a base parameter, and `38;5;1:7` would
            // otherwise promote the trailing 7 into SGR 7, reverse video.
            while (next < params.count && params.subparam[next]) {
                ++next;
            }
            break;
        }
        case 59:  // reset underline color to "follow the foreground"
            pen.ul = {};
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

bool handleErase(Grid& grid, const Params& params, std::span<const std::uint8_t> intermediates,
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
                grid.cellAt(r, c) = Cell{};
            }
            if (c0 == 0 && c1 == grid.cols - 1) {
                // A fully-blanked row is no longer a wrap continuation;
                // leaving the flag would glue blanks onto the previous
                // logical line at reflow time.
                grid.lineAt(r).wrappedFromPrev = false;
            }
            if (c1 == grid.cols - 1 && r + 1 < grid.rows) {
                // Erasing to end-of-line severs the join to the next row
                // (xterm ClearRight -> LineClrWrapped).
                grid.lineAt(r + 1).wrappedFromPrev = false;
            }
            grid.damage.mark(r, c0, c1);
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
        if (mode >= 0 && mode <= 2) {
            grid.pendingWrap = false;  // DEC STD 070: erase resets the LCF
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
        if (mode >= 0 && mode <= 2) {
            grid.pendingWrap = false;  // DEC STD 070: erase resets the LCF
        }
        return true;
    default:
        return false;
    }
}

}  // namespace krait::core::vt
