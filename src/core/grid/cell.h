#pragma once

#include <cstdint>

namespace krait::core::vt {

// Pen/cell attributes for SGR basic (T7). Extended color (38/48 truecolor,
// 256-index, 58/59 underline color) is M1 — Color::Kind already reserves
// room so the struct does not need to change shape then.
struct Color {
    enum class Kind : std::uint8_t { Default, Indexed, Rgb };
    Kind kind = Kind::Default;
    std::uint8_t index = 0;  // 0-15 from SGR 30-37/90-97 (and 256-color later)
    std::uint32_t rgb = 0;   // 0xRRGGBB when kind == Rgb (M1)

    friend bool operator==(const Color&, const Color&) = default;
};

struct Attr {
    enum Flag : std::uint16_t {
        kBold = 1U << 0,
        kDim = 1U << 1,
        kItalic = 1U << 2,
        kUnderline = 1U << 3,
        kBlink = 1U << 4,
        kReverse = 1U << 5,
        kInvisible = 1U << 6,
        kStrike = 1U << 7,
    };

    std::uint16_t flags = 0;
    Color fg;
    Color bg;

    friend bool operator==(const Attr&, const Attr&) = default;
};

// One grid cell. ch == 0 means "never written / erased" (erasure resets the
// cell to defaults; BCE is undecided until the real grid, see conformance).
struct Cell {
    char32_t ch = 0;
    Attr attr;
};

}  // namespace krait::core::vt
