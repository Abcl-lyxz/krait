#include "core/graphics/sixel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace krait::core::vt {
namespace {

constexpr std::uint32_t kOpaque = 0xFF000000U;

std::uint32_t pack(std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    return kOpaque | (r << 16) | (g << 8) | b;
}

// 0-100 percent to 0-255. Rounded rather than truncated: 100% must be 255 and
// not 254, or every white pixel in every sixel image is imperceptibly grey and
// a golden-image test drifts by one for no reason anybody can find.
std::uint32_t percentToByte(std::uint32_t percent) {
    const std::uint32_t clamped = std::min<std::uint32_t>(percent, 100);
    return (clamped * 255 + 50) / 100;
}

// The VT340's default map. libsixel ships the same sixteen, and img2sixel's
// output relies on them whenever it does not define a colour it uses.
constexpr std::array<std::array<std::uint32_t, 3>, 16> kDefaultPercent{{
    {0, 0, 0},
    {20, 20, 80},
    {80, 13, 13},
    {20, 80, 20},
    {80, 20, 80},
    {20, 80, 80},
    {80, 80, 20},
    {53, 53, 53},
    {26, 26, 26},
    {33, 33, 60},
    {60, 26, 26},
    {33, 60, 33},
    {60, 33, 60},
    {33, 60, 60},
    {60, 60, 33},
    {80, 80, 80},
}};

const std::array<std::uint32_t, 16>& defaultPalette() {
    static const std::array<std::uint32_t, 16> kTable = [] {
        std::array<std::uint32_t, 16> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] =
                pack(percentToByte(kDefaultPercent[i][0]), percentToByte(kDefaultPercent[i][1]),
                     percentToByte(kDefaultPercent[i][2]));
        }
        return out;
    }();
    return kTable;
}

}  // namespace

std::span<const std::uint32_t> sixelDefaultPalette() {
    return defaultPalette();
}

std::uint32_t sixelHlsToRgb(int hue, int lightness, int saturation) {
    // DEC's hue origin is 120 degrees off the usual one: in the VT340's model
    // 0 degrees is BLUE, not red. libsixel carries the same rotation, and
    // getting it wrong swaps the primaries in every HLS image — which looks
    // like a working decoder that simply has odd taste.
    const double h = static_cast<double>((((hue % 360) + 360) % 360) + 240) / 60.0;
    const double l = std::clamp(lightness, 0, 100) / 100.0;
    const double s = std::clamp(saturation, 0, 100) / 100.0;

    if (s <= 0.0) {
        const auto grey = static_cast<std::uint32_t>(std::lround(l * 255.0));
        return pack(grey, grey, grey);
    }
    const double m2 = l <= 0.5 ? l * (1.0 + s) : l + s - l * s;
    const double m1 = 2.0 * l - m2;
    const auto channel = [m1, m2](double t) {
        if (t < 0.0) {
            t += 6.0;
        }
        if (t >= 6.0) {
            t -= 6.0;
        }
        double value = m1;
        if (t < 1.0) {
            value = m1 + (m2 - m1) * t;
        } else if (t < 3.0) {
            value = m2;
        } else if (t < 4.0) {
            value = m1 + (m2 - m1) * (4.0 - t);
        }
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };
    return pack(channel(h + 2.0), channel(h), channel(h - 2.0));
}

void SixelDecoder::begin(const Params& params) noexcept {
    m_image = Image{};
    const std::array<std::uint32_t, 16>& defaults = defaultPalette();
    for (std::size_t i = 0; i < kPaletteSize; ++i) {
        // Entries past 15 start as the default map repeated rather than as
        // transparent black: a file that selects colour 200 without defining it
        // is malformed, and painting it invisibly hides the bug from whoever
        // has to explain the blank rectangle.
        m_palette[i] = defaults[i % defaults.size()];
    }
    resetParams();
    m_state = State::Ground;
    m_color = 0;
    m_x = 0;
    m_bandTop = 0;
    m_maxX = 0;
    m_declaredWidth = 0;
    m_declaredHeight = 0;
    m_overflowed = false;
    m_wroteAnything = false;
    // P2 == 1 is the only value that changes anything: "pixel positions
    // specified as 0 remain at their current color". Every other value, and an
    // absent parameter, paints the background.
    m_transparentBackground = params.count >= 2 && params.values[1] == 1;
}

void SixelDecoder::resetParams() noexcept {
    m_params.fill(0);
    m_paramCount = 0;
    m_paramPending = false;
}

void SixelDecoder::pushParam(std::uint32_t value) {
    if (m_paramCount < m_params.size()) {
        m_params[m_paramCount] = value;
    }
    // Counted even when it did not fit, so a sender cannot smuggle a sixth
    // parameter into the fifth slot by padding the list.
    ++m_paramCount;
}

std::uint32_t SixelDecoder::backgroundFill() const noexcept {
    // What a pixel the data never writes ends up as, decided once by P2 rather
    // than re-litigated per band. P2 == 1 says a zero bit leaves the pixel
    // alone, which for a fresh canvas means transparent; every other value
    // (and an absent parameter) means the background colour, and the
    // background colour on a fresh canvas is opaque black.
    return m_transparentBackground ? 0U : kOpaque;
}

bool SixelDecoder::ensureSize(int width, int height) {
    if (m_overflowed) {
        return false;
    }
    if (width > kMaxImageDimension || height > kMaxImageDimension) {
        m_overflowed = true;
        return false;
    }
    const int newWidth = std::max(m_image.width, width);
    const int newHeight = std::max(m_image.height, height);
    if (newWidth == m_image.width && newHeight == m_image.height) {
        return true;
    }
    const auto pixels = static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight);
    if (pixels > kMaxImagePixels) {
        m_overflowed = true;
        return false;
    }

    // Row-preserving grow. A plain resize() would work only while the width is
    // unchanged; sixel grows in BOTH directions as it goes, and a naive resize
    // silently shears the image one row further right on every widening.
    std::vector<std::uint32_t> grown(pixels, backgroundFill());
    for (int y = 0; y < m_image.height; ++y) {
        const auto src = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_image.width);
        const auto dst = static_cast<std::size_t>(y) * static_cast<std::size_t>(newWidth);
        std::copy_n(m_image.pixels.begin() + static_cast<std::ptrdiff_t>(src),
                    static_cast<std::size_t>(m_image.width),
                    grown.begin() + static_cast<std::ptrdiff_t>(dst));
    }
    m_image.pixels = std::move(grown);
    m_image.width = newWidth;
    m_image.height = newHeight;
    return true;
}

void SixelDecoder::applyRasterAttributes() {
    // " Pan ; Pad ; Ph ; Pv. The aspect ratio is read and discarded for the
    // same reason P1 is; Ph x Pv is the useful half, and pre-sizing on it means
    // one allocation instead of a grow per band.
    if (m_paramCount >= 4) {
        m_declaredWidth =
            static_cast<int>(std::min<std::uint32_t>(m_params[2], kMaxImageDimension));
        m_declaredHeight =
            static_cast<int>(std::min<std::uint32_t>(m_params[3], kMaxImageDimension));
        if (m_declaredWidth > 0 && m_declaredHeight > 0) {
            ensureSize(m_declaredWidth, m_declaredHeight);
        }
    }
    resetParams();
}

void SixelDecoder::selectOrDefineColor() {
    if (m_paramCount == 0) {
        resetParams();
        return;
    }
    const std::uint32_t index = m_params[0] % kPaletteSize;
    if (m_paramCount >= 5) {
        // Definition. Pu 1 = HLS, 2 = RGB; anything else is a sender that means
        // something this format does not have, so the colour is left alone
        // rather than guessed at.
        if (m_params[1] == 2) {
            m_palette[index] = pack(percentToByte(m_params[2]), percentToByte(m_params[3]),
                                    percentToByte(m_params[4]));
        } else if (m_params[1] == 1) {
            m_palette[index] =
                sixelHlsToRgb(static_cast<int>(m_params[2]), static_cast<int>(m_params[3]),
                              static_cast<int>(m_params[4]));
        }
    }
    // Selection happens either way: `#1;2;100;0;0` both defines colour 1 and
    // selects it, which is what every encoder relies on.
    m_color = index;
    resetParams();
}

void SixelDecoder::emitSixel(std::uint8_t bits, int repeat) {
    if (repeat <= 0) {
        return;
    }
    // SATURATING, not wrapping. `repeat` is capped at 2^24 by the parameter
    // parser, so a hundred-odd `!16777215~` commands — about a kilobyte of
    // input — would carry a plain `m_x + repeat` past INT_MAX, and signed
    // overflow is undefined behaviour reachable from a remote host. That is a
    // security bug by rules/vt-core.md's definition, not a cosmetic one.
    const int endX = repeat > kMaxImageDimension - m_x ? kMaxImageDimension + 1 : m_x + repeat;
    if (!ensureSize(endX, m_bandTop + 6)) {
        // Still ADVANCE the position, up to the saturation point. A bound is
        // not a reason to start drawing the rest of the image in the wrong
        // place, and the sender is going to keep sending either way.
        m_x = endX;
        return;
    }

    const std::uint32_t colour = m_palette[m_color];
    for (int bit = 0; bit < 6; ++bit) {
        // LSB is the TOP pixel. Reversing this is the other classic sixel bug:
        // the image decodes, looks almost right, and is vertically mirrored
        // within every six-pixel band.
        if ((bits & (1U << bit)) == 0) {
            // A ZERO BIT WRITES NOTHING, whatever P2 says, and that is a
            // deliberate divergence from the letter of the spec ("pixel
            // positions specified as 0 are set to the current background
            // color"). Taken literally, overprinting is impossible: a
            // multi-colour image is built by returning to the left with `$` and
            // laying down another colour, and every one of those passes would
            // erase the pass before it with background. Since overprinting IS
            // how every encoder produces a colour image, the literal reading
            // cannot be the intended one — and libsixel and xterm both do what
            // this does. P2 is honoured where it actually means something: it
            // decides what an UNWRITTEN pixel is (see backgroundFill()), which
            // is the same question asked once at the start instead of on every
            // band.
            continue;
        }
        const int y = m_bandTop + bit;
        const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_image.width);
        for (int x = m_x; x < endX; ++x) {
            m_image.pixels[row + static_cast<std::size_t>(x)] = colour;
        }
    }
    m_x = endX;
    m_maxX = std::max(m_maxX, endX);
    m_wroteAnything = true;
}

void SixelDecoder::put(std::uint8_t byte) {
    // Parameter digits and separators are shared by all three parameterised
    // commands, so they are handled once, before the per-state dispatch.
    if (m_state != State::Ground) {
        if (byte >= '0' && byte <= '9') {
            if (!m_paramPending) {
                pushParam(0);
                m_paramPending = true;
            }
            if (m_paramCount > 0 && m_paramCount <= m_params.size()) {
                std::uint32_t& slot = m_params[m_paramCount - 1];
                // Clamped rather than wrapped. A run of digits long enough to
                // overflow is hostile input, and a wrapped repeat count is a
                // small number that draws the wrong picture instead of a large
                // one that is refused.
                slot = std::min<std::uint32_t>(slot * 10 + (byte - '0'), 1U << 24);
            }
            return;
        }
        if (byte == ';') {
            if (!m_paramPending) {
                pushParam(0);  // an omitted value is 0, per the spec
            }
            m_paramPending = false;
            return;
        }
        // Anything else ends the parameter list and is re-dispatched below.
        switch (m_state) {
        case State::Color:
            selectOrDefineColor();
            break;
        case State::Raster:
            applyRasterAttributes();
            break;
        case State::Repeat: {
            const int count = m_paramCount > 0 ? static_cast<int>(m_params[0]) : 1;
            resetParams();
            m_state = State::Ground;
            if (byte >= 0x3F && byte <= 0x7E) {
                emitSixel(static_cast<std::uint8_t>(byte - 0x3F), count);
            }
            return;  // the repeated character is consumed by the repeat
        }
        case State::Ground:
            break;
        }
        m_state = State::Ground;
        // and fall through to the ground dispatch for this same byte
    }

    switch (byte) {
    case '#':
        m_state = State::Color;
        resetParams();
        return;
    case '!':
        m_state = State::Repeat;
        resetParams();
        return;
    case '"':
        m_state = State::Raster;
        resetParams();
        return;
    case '$':
        m_x = 0;
        return;
    case '-':
        m_x = 0;
        // Saturating for the same reason as emitSixel's endX above. This one
        // needs ~360 MB of `-` bytes to overflow rather than a kilobyte, which
        // makes it the less urgent half and not a different problem.
        if (m_bandTop <= kMaxImageDimension) {
            m_bandTop += 6;
        }
        return;
    default:
        break;
    }
    if (byte >= 0x3F && byte <= 0x7E) {
        emitSixel(static_cast<std::uint8_t>(byte - 0x3F), 1);
    }
    // Everything else — CR, LF, the spaces encoders insert to wrap long lines —
    // is ignored rather than fatal. The format has no escaping, so a decoder
    // that rejected unknown bytes would reject half the files in the world.
}

std::optional<Image> SixelDecoder::end(bool aborted) {
    // A command still mid-parameters at the terminator is completed first: a
    // trailing `#3` with no data after it is legal and means "select colour 3".
    if (m_state == State::Color) {
        selectOrDefineColor();
    } else if (m_state == State::Raster) {
        applyRasterAttributes();
    }
    m_state = State::Ground;

    if (aborted || m_overflowed || !m_wroteAnything || m_image.empty()) {
        m_image = Image{};
        return std::nullopt;
    }

    // Crop to what was actually written, unless the raster attributes declared
    // a size — in which case the DECLARATION wins, because an encoder that
    // states a width means it even for a final band that ends early, and
    // cropping to the last written pixel would shrink an image with a
    // transparent right edge.
    const int width = m_declaredWidth > 0 ? std::min(m_declaredWidth, m_image.width)
                                          : std::min(m_maxX, m_image.width);
    const int height =
        m_declaredHeight > 0 ? std::min(m_declaredHeight, m_image.height) : m_image.height;
    if (width <= 0 || height <= 0) {
        m_image = Image{};
        return std::nullopt;
    }
    if (width == m_image.width && height == m_image.height) {
        return std::exchange(m_image, Image{});
    }

    Image cropped;
    cropped.width = width;
    cropped.height = height;
    cropped.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const auto src = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_image.width);
        const auto dst = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        std::copy_n(m_image.pixels.begin() + static_cast<std::ptrdiff_t>(src),
                    static_cast<std::size_t>(width),
                    cropped.pixels.begin() + static_cast<std::ptrdiff_t>(dst));
    }
    m_image = Image{};
    return cropped;
}

}  // namespace krait::core::vt
