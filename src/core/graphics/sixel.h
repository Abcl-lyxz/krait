#pragma once

#include "core/graphics/image.h"
#include "core/parser/events.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace krait::core::vt {

// Sixel decoder (M5 T79), DEC STD 070 / VT330-VT340 chapter 14.
//
// Wire form: DCS P1 ; P2 ; P3 q <data> ST.
//   P1  pixel aspect ratio. Legacy, and IGNORED here: every encoder in use
//       emits square pixels and states the size in the raster attributes
//       instead, so honouring P1 would stretch modern output that means
//       nothing by it.
//   P2  background select. 1 means a zero bit leaves the pixel ALONE, which
//       for us means transparent; 0 and 2 mean it is painted the background
//       colour. This is the only one of the three that changes what is drawn.
//   P3  horizontal grid size. The VT300 ignores it and so do we.
//
// The data alphabet:
//   '?'..'~'   six vertical pixels, value = byte - 0x3F, **LSB at the TOP**
//   '#'Pc      select colour Pc
//   '#'Pc;Pu;Px;Py;Pz  define colour Pc. Pu 1 = HLS, 2 = RGB. The components
//              are PERCENTAGES, 0-100 — not 0-255, which is the single most
//              common way to get sixel wrong and turns every image dark.
//   '!'Pn c    repeat character c, Pn times
//   '$'        graphics carriage return: back to the left of this band
//   '-'        graphics new line: down one band (six pixels), back to the left
//   '"'Pan;Pad;Ph;Pv   raster attributes; Ph x Pv is the image size
//
// The decoder streams, because the parser holds no DCS buffer (machine.h
// streams payload bytes) and because a sixel image is the largest thing a
// remote host can ask this terminal to allocate — buffering the text form
// first would mean paying for it twice.
class SixelDecoder {
  public:
    // DCS parameters. Only P2 is read; see above.
    void begin(const Params& params) noexcept;

    void put(std::uint8_t byte);

    // The finished image, or nullopt when the string was aborted, was empty, or
    // hit a bound. An aborted sixel yields nothing at all rather than a partial
    // image: half a picture drawn over the user's terminal is worse than none,
    // and xterm discards aborted strings across the board.
    std::optional<Image> end(bool aborted);

    // True once a bound was hit. The decoder keeps CONSUMING after that — the
    // parser still has to find the terminator — but stops writing pixels, so a
    // hostile sender pays parse time and no memory.
    bool overflowed() const noexcept { return m_overflowed; }

  private:
    void selectOrDefineColor();
    void emitSixel(std::uint8_t bits, int repeat);
    bool ensureSize(int width, int height);
    std::uint32_t backgroundFill() const noexcept;
    void applyRasterAttributes();
    void pushParam(std::uint32_t value);
    void resetParams() noexcept;

    // 256 entries is the format's own limit ("Pc: 0 to 255").
    static constexpr std::size_t kPaletteSize = 256;

    enum class State : std::uint8_t {
        Ground,
        Color,   // after '#'
        Repeat,  // after '!'
        Raster,  // after '"'
    };

    std::array<std::uint32_t, kPaletteSize> m_palette{};
    Image m_image;
    // Parameters of the command being read. Sixel's own parameter syntax, not
    // the CSI parser's: it appears INSIDE the DCS payload, so the machine never
    // sees it.
    std::array<std::uint32_t, 8> m_params{};
    std::size_t m_paramCount = 0;
    bool m_paramPending = false;

    State m_state = State::Ground;
    std::uint32_t m_color = 0;  // current palette index
    int m_x = 0;                // pixel column of the active position
    int m_bandTop = 0;          // pixel row of the current band
    int m_maxX = 0;             // rightmost pixel written, for the crop
    int m_declaredWidth = 0;    // raster attributes; 0 when absent
    int m_declaredHeight = 0;
    bool m_transparentBackground = false;  // P2 == 1
    bool m_overflowed = false;
    bool m_wroteAnything = false;
};

// The VT340's sixteen default colours, as 0xAARRGGBB. Exposed for the tests,
// which is the only way to assert that an image using them decoded correctly —
// a file that defines every colour it uses never touches this table, and most
// do, so without a test it would rot unnoticed.
std::span<const std::uint32_t> sixelDefaultPalette();

// HLS to RGB with sixel's ranges: hue 0-360 DEGREES, lightness and saturation
// 0-100 PERCENT. Exposed because it is the part of this format people get
// wrong twice — once on the ranges, and once on DEC's hue origin, which is not
// the usual one.
std::uint32_t sixelHlsToRgb(int hue, int lightness, int saturation);

}  // namespace krait::core::vt
