#pragma once

#include <cstdint>

namespace krait::core::vt {

// Pen/cell attributes. One Color type serves fg, bg and the underline color
// because SGR 58 "works exactly like the codes 38, 48" (kitty).
struct Color {
    enum class Kind : std::uint8_t { Default, Indexed, Rgb };
    Kind kind = Kind::Default;
    std::uint8_t index = 0;  // 0-15 from SGR 30-37/90-97, 0-255 from 38:5:Ps
    std::uint32_t rgb = 0;   // 0xRRGGBB when kind == Rgb

    friend bool operator==(const Color&, const Color&) = default;
};

// Underline styles: SGR 4:0-4:5 (kitty extension — xterm's ctlseqs documents
// plain `4` only). NOT an Attr::Flag bit: the styles are mutually exclusive,
// and SGR 21 selects Double rather than setting a second flag.
enum class Underline : std::uint8_t {
    None = 0,
    Single = 1,
    Double = 2,
    Curly = 3,
    Dotted = 4,
    Dashed = 5,
};

struct Attr {
    enum Flag : std::uint16_t {
        kBold = 1U << 0,
        kDim = 1U << 1,
        kItalic = 1U << 2,
        // Bit 3 held kUnderline until T17; underline is a style, see below.
        kBlink = 1U << 4,
        kReverse = 1U << 5,
        kInvisible = 1U << 6,
        kStrike = 1U << 7,
    };

    std::uint16_t flags = 0;
    Underline underline = Underline::None;
    Color fg;
    Color bg;
    // Underline color. Kind::Default means "follow the foreground": kitty
    // requires a SET underline color to survive reverse video unchanged, and
    // only an unset one to track fg — so this cannot collapse into fg.
    Color ul;

    friend bool operator==(const Attr&, const Attr&) = default;
};

// One grid cell. ch == 0 means "never written / erased" (erasure resets the
// cell to defaults; BCE is undecided until the real grid, see conformance).
//
// ponytail: T17 grew Attr 20 -> 28 bytes (three 8-byte Colors), so Cell went
// 24 -> 32. Cell is the scrollback storage unit: at kMaxScrollback (10k lines)
// x 240 columns that is ~19 MB of added resident memory. Packing Color into a
// single uint32 (kind in the high byte, index-or-rgb in the low 24 — they are
// mutually exclusive) would give Color 4 bytes, Attr 16, Cell 20, i.e. SMALLER
// than before T17 while carrying more state. Deliberately not done here: T21
// owns scrollback memory and has the "flood keeps O(cap) memory" test to prove
// the win, and this struct's bounds behaviour was just audited in its current
// shape. Do it in T21, measure it there.
struct Cell {
    char32_t ch = 0;
    Attr attr;
};

}  // namespace krait::core::vt
