#include "remote_text.h"

namespace krait::net {

QString sanitizeRemoteText(std::string_view text, std::size_t maxChars, int maxLines) {
    // Bound the INPUT before decoding, not just the output: a server that sends
    // ten megabytes should cost us a truncation, not a ten-megabyte QString on
    // its way to being shortened.
    const std::size_t inputCap = maxChars * 4 + 4;  // 4 bytes is the widest UTF-8
    const bool inputClipped = text.size() > inputCap;
    if (inputClipped) {
        text = text.substr(0, inputCap);
    }

    // Invalid UTF-8 becomes replacement characters rather than being dropped.
    // Silently deleting bytes lets a server hide characters from the user while
    // still knowing what it sent.
    const QString decoded = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));

    QString out;
    out.reserve(static_cast<qsizetype>(maxChars));
    int lines = 1;
    bool clipped = inputClipped;

    for (const QChar ch : decoded) {
        if (static_cast<std::size_t>(out.size()) >= maxChars) {
            clipped = true;
            break;
        }
        if (ch == u'\n') {
            if (++lines > maxLines) {
                clipped = true;
                break;
            }
            out += ch;
            continue;
        }
        // C0 (including \r and \t, which a banner has no use for), DEL, and C1.
        // C1 matters as much as C0: 0x9B is CSI on its own in some decodings.
        const char16_t code = ch.unicode();
        if (code < 0x20 || code == 0x7F || (code >= 0x80 && code <= 0x9F)) {
            continue;
        }
        out += ch;
    }

    if (clipped) {
        out += QChar(0x2026);  // …
    }
    return out;
}

}  // namespace krait::net
