#include "core/unicode/utf8.h"

namespace krait::core {

namespace {
constexpr char32_t kReplacement = U'�';
}

int Utf8Decoder::feed(std::uint8_t byte, char32_t (&out)[2]) noexcept {
    int produced = 0;
    bool reprocess = true;
    while (reprocess) {
        reprocess = false;
        if (m_bytesNeeded == 0) {
            if (byte <= 0x7F) {
                out[produced++] = static_cast<char32_t>(byte);
            } else if (byte >= 0xC2 && byte <= 0xDF) {
                m_bytesNeeded = 1;
                m_codepoint = byte & 0x1F;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                if (byte == 0xE0) {
                    m_lowerBoundary = 0xA0;
                }
                if (byte == 0xED) {
                    m_upperBoundary = 0x9F;
                }
                m_bytesNeeded = 2;
                m_codepoint = byte & 0x0F;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                if (byte == 0xF0) {
                    m_lowerBoundary = 0x90;
                }
                if (byte == 0xF4) {
                    m_upperBoundary = 0x8F;
                }
                m_bytesNeeded = 3;
                m_codepoint = byte & 0x07;
            } else {
                out[produced++] = kReplacement;
            }
        } else if (byte >= m_lowerBoundary && byte <= m_upperBoundary) {
            m_lowerBoundary = 0x80;
            m_upperBoundary = 0xBF;
            m_codepoint = (m_codepoint << 6) | (byte & 0x3F);
            if (--m_bytesNeeded == 0) {
                out[produced++] = m_codepoint;
                m_codepoint = 0;
            }
        } else {
            // Malformed: emit U+FFFD and restore the byte to the stream.
            m_codepoint = 0;
            m_bytesNeeded = 0;
            m_lowerBoundary = 0x80;
            m_upperBoundary = 0xBF;
            out[produced++] = kReplacement;
            reprocess = true;  // terminates: next pass takes the start branch
        }
    }
    return produced;
}

int Utf8Decoder::finish(char32_t (&out)[1]) noexcept {
    const bool pending = m_bytesNeeded != 0;
    m_codepoint = 0;
    m_bytesNeeded = 0;
    m_lowerBoundary = 0x80;
    m_upperBoundary = 0xBF;
    if (pending) {
        out[0] = kReplacement;
        return 1;
    }
    return 0;
}

}  // namespace krait::core
