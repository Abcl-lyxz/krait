#include "hostkey_art.h"

#include <vector>

namespace krait::net {

namespace {

// OpenSSH's constants (sshkey_fingerprint_randomart). FLDBASE 8 gives a field
// 17 wide and 9 tall, which is why the box below is 19 characters across.
constexpr int kFieldBase = 8;
constexpr int kFieldX = kFieldBase * 2 + 1;  // 17
constexpr int kFieldY = kFieldBase + 1;      // 9

// Density -> character. Index 15 is the start square, 16 the end square, so a
// visit count saturates at 14 ('^') and cannot be mistaken for either.
constexpr std::string_view kSymbols = " .o+=*BOX@%&#/^SE";
constexpr int kStartSymbol = 15;
constexpr int kEndSymbol = 16;
constexpr int kMaxCount = kEndSymbol - 2;  // 14

void appendBorder(std::string& out, std::string_view label) {
    // The label is bracketed and centred, and truncated if it will not fit — a
    // title long enough to break the box would make the art unreadable, and the
    // art is the point.
    std::string text;
    text.reserve(label.size() + 2);
    text += '[';
    text.append(label.substr(0, static_cast<std::size_t>(kFieldX) - 2));
    text += ']';

    const int labelLen = static_cast<int>(text.size());
    const int left = (kFieldX - labelLen) / 2;

    out += '+';
    out.append(static_cast<std::size_t>(left), '-');
    out += text;
    out.append(static_cast<std::size_t>(kFieldX - left - labelLen), '-');
    out += '+';
}

}  // namespace

std::string randomart(const unsigned char* digest, std::size_t length, std::string_view title,
                      std::string_view hashName) {
    std::vector<int> field(static_cast<std::size_t>(kFieldX) * kFieldY, 0);
    const auto at = [&field](int x, int y) -> int& {
        return field[static_cast<std::size_t>(y) * kFieldX + x];
    };

    // The bishop starts in the middle of the room.
    int x = kFieldX / 2;
    int y = kFieldY / 2;
    const int startX = x;
    const int startY = y;

    for (std::size_t i = 0; i < length; ++i) {
        unsigned char byte = digest[i];
        // Four moves per byte, two bits at a time, LOW bits first. Bit 0 picks
        // right vs left, bit 1 picks down vs up.
        for (int step = 0; step < 4; ++step) {
            x += (byte & 0x1) != 0 ? 1 : -1;
            y += (byte & 0x2) != 0 ? 1 : -1;

            // The walls stop him; they do not wrap. Wrapping would let two very
            // different keys land on the same picture far too often.
            x = x < 0 ? 0 : (x > kFieldX - 1 ? kFieldX - 1 : x);
            y = y < 0 ? 0 : (y > kFieldY - 1 ? kFieldY - 1 : y);

            if (at(x, y) < kMaxCount) {
                ++at(x, y);
            }
            byte >>= 2;
        }
    }
    at(startX, startY) = kStartSymbol;
    at(x, y) = kEndSymbol;

    std::string out;
    out.reserve(static_cast<std::size_t>(kFieldX + 3) * (kFieldY + 2));
    appendBorder(out, title);
    for (int row = 0; row < kFieldY; ++row) {
        out += "\n|";
        for (int col = 0; col < kFieldX; ++col) {
            out += kSymbols[static_cast<std::size_t>(at(col, row))];
        }
        out += '|';
    }
    out += '\n';
    appendBorder(out, hashName);
    return out;
}

}  // namespace krait::net
