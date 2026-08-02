#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace krait::core::vt {

// A decoded image, in the one representation both graphics protocols land in
// and the renderer uploads (M5 T79/T80).
//
// 0xAARRGGBB, straight alpha, top row first. NOT premultiplied: sixel's
// transparency is a whole-pixel "this one was never written" rather than a
// blend, and premultiplying would throw away the colour of a fully transparent
// pixel a later pass might still reveal.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;

    bool empty() const noexcept { return width <= 0 || height <= 0 || pixels.empty(); }

    std::size_t byteSize() const noexcept { return pixels.size() * sizeof(std::uint32_t); }
};

// Bounds shared by both decoders. Remote input is hostile (rules/net.md), and
// an image is the one thing in the protocol whose declared size is trusted for
// an allocation — so it is not trusted.
//
// The dimension caps are generous because a legitimate screenshot pasted into a
// terminal really is a few thousand pixels wide. The PIXEL cap is the one that
// matters: 8192 x 8192 declared is 256 MiB at four bytes a pixel, and a sender
// can declare it in twenty bytes.
inline constexpr int kMaxImageDimension = 8192;
inline constexpr std::size_t kMaxImagePixels = 4u * 1024 * 1024;  // 16 MiB decoded

}  // namespace krait::core::vt
