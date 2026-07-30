#pragma once

#include <cstdint>

namespace krait::core::vt {

// Pen/cell attributes. One Color type serves fg, bg and the underline color
// because SGR 58 "works exactly like the codes 38, 48" (kitty).
// Packed into one uint32: the kind in the high byte, the payload in the low 24.
// An index (0-255) and a 24-bit RGB are mutually exclusive, so they never need
// to coexist and the old {kind, index, rgb} aggregate was paying 8 bytes to
// store 4. Three of these live in every Attr, so the saving is x3 per Cell.
//
// T21 did this, as the T17 note below asked. Measured: Color 8 -> 4, Attr
// 28 -> 16, Cell 32 -> 20, i.e. SMALLER than before T17 while carrying more
// state. A static_assert below pins it, because the whole point is a memory
// bound and a silent regrowth would give that up without anyone noticing.
class Color {
  public:
    enum class Kind : std::uint8_t { Default, Indexed, Rgb };

    constexpr Color() = default;

    // 0-15 from SGR 30-37/90-97, 0-255 from 38:5:Ps.
    static constexpr Color indexed(std::uint8_t i) {
        return Color{static_cast<std::uint32_t>(Kind::Indexed) << kKindShift | i};
    }

    // 0xRRGGBB.
    static constexpr Color rgb(std::uint32_t value) {
        return Color{static_cast<std::uint32_t>(Kind::Rgb) << kKindShift | (value & kPayloadMask)};
    }

    constexpr Kind kind() const { return static_cast<Kind>(m_bits >> kKindShift); }

    constexpr std::uint8_t index() const { return static_cast<std::uint8_t>(m_bits & 0xFFU); }

    constexpr std::uint32_t rgb() const { return m_bits & kPayloadMask; }

    friend bool operator==(const Color&, const Color&) = default;

  private:
    static constexpr int kKindShift = 24;
    static constexpr std::uint32_t kPayloadMask = 0x00FF'FFFFU;

    constexpr explicit Color(std::uint32_t bits) : m_bits(bits) {}

    std::uint32_t m_bits = 0;  // Kind::Default with a zero payload
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

// Unicode's scalar range stops at U+10FFFF, so the top of char32_t's 32 bits is
// permanently unreachable by any legal codepoint. T20 spends two of those spare
// bits rather than growing Cell, which is what keeps the T17 ponytail note below
// achievable: cluster storage and wide cells cost ZERO extra bytes, so T21 can
// still pack Color and land Cell at 20.
//
// The three states a written `ch` can be in:
//   ch <= 0x10FFFF      a literal single-codepoint cluster (the common case)
//   ch == kWideTrailing the right-hand cell of a 2-column cluster. Owns nothing;
//                       the cluster itself lives in the cell to its LEFT.
//   ch &  kClusterTag   a multi-codepoint cluster; the low bits index the
//                       Grid's ClusterPool.
// Zero still means "never written" and is none of the three.
inline constexpr char32_t kWideTrailing = 0x8000'0000U;
inline constexpr char32_t kClusterTag = 0x4000'0000U;
inline constexpr char32_t kClusterMask = 0x3FFF'FFFFU;

constexpr bool isWideTrailing(char32_t ch) noexcept {
    return ch == kWideTrailing;
}

constexpr bool isClusterRef(char32_t ch) noexcept {
    return (ch & kClusterTag) != 0;
}

constexpr std::uint32_t clusterRefIndex(char32_t ch) noexcept {
    return static_cast<std::uint32_t>(ch & kClusterMask);
}

// One grid cell. ch == 0 means "never written / erased" (erasure resets the
// cell to defaults; BCE is undecided until the real grid, see conformance).
//
// T17 grew Attr to 28 bytes and Cell to 32; T21 packed Color and brought Cell
// to 20, which is 4 bytes SMALLER than it was before T17 despite carrying
// truecolor, underline colors and underline styles. At the 10k-line scrollback
// cap x 240 columns that is ~29 MB less resident memory than the T17 shape.
struct Cell {
    char32_t ch = 0;
    Attr attr;
};

// Cell is the scrollback storage unit, so its size IS the memory bound — every
// byte here is multiplied by the line cap times the column count. Pinned so a
// future field cannot quietly give back what T21 measured.
static_assert(sizeof(Color) == 4, "Color must stay packed into one uint32");
static_assert(sizeof(Cell) == 20, "Cell grew — see the scrollback memory bound");

}  // namespace krait::core::vt
