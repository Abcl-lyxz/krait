#pragma once

#include <cstdint>

namespace krait::core {

// Incremental UTF-8 decoder implementing the WHATWG Encoding Standard
// algorithm: malformed input is replaced with U+FFFD, with the offending
// byte restored to the stream (so one fed byte can emit up to two scalar
// values). Allocation-free; safe on hostile input.
class Utf8Decoder {
  public:
    // Decodes one byte. Appends 0..2 scalar values to `out`; returns count.
    int feed(std::uint8_t byte, char32_t (&out)[2]) noexcept;

    // Signals end-of-stream. Emits U+FFFD into `out` and returns 1 if a
    // sequence was pending; otherwise returns 0. Resets the decoder.
    int finish(char32_t (&out)[1]) noexcept;

  private:
    char32_t m_codepoint = 0;
    std::uint8_t m_bytesNeeded = 0;
    std::uint8_t m_lowerBoundary = 0x80;
    std::uint8_t m_upperBoundary = 0xBF;
};

}  // namespace krait::core
