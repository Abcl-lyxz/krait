#include "remote_text.h"
#include <catch2/catch_test_macros.hpp>

#include <string>

using krait::net::sanitizeRemoteText;

TEST_CASE("plain text survives unchanged", "[net][remote]") {
    CHECK(sanitizeRemoteText("Password:") == QStringLiteral("Password:"));
    CHECK(sanitizeRemoteText("") == QString());
    // Newlines are the one control that stays: a keyboard-interactive
    // instruction block is genuinely multi-line.
    CHECK(sanitizeRemoteText("One\nTwo") == QStringLiteral("One\nTwo"));
}

TEST_CASE("controls that could rewrite the banner are removed", "[net][remote]") {
    // ESC is the whole point: a server that can put ESC [ 2 J in a prompt can
    // rewrite whatever the UI drew around it.
    CHECK(sanitizeRemoteText("a\x1b[2Jb") == QStringLiteral("a[2Jb"));
    CHECK(sanitizeRemoteText("a\rb") == QStringLiteral("ab"));
    CHECK(sanitizeRemoteText("a\tb") == QStringLiteral("ab"));
    CHECK(sanitizeRemoteText(std::string("a\0b", 3)) == QStringLiteral("ab"));
    CHECK(sanitizeRemoteText("a\x7f"
                             "b") == QStringLiteral("ab"));
    // C1 matters as much as C0: 0x9B is CSI on its own once decoded.
    CHECK(sanitizeRemoteText("a\xc2\x9b"
                             "2Jb") == QStringLiteral("a2Jb"));
}

TEST_CASE("length and line count are bounded", "[net][remote]") {
    const std::string long_(4096, 'x');
    const QString clipped = sanitizeRemoteText(long_, 32, 12);
    CHECK(clipped.size() == 33);  // 32 plus the ellipsis
    CHECK(clipped.endsWith(QChar(0x2026)));

    std::string many;
    for (int i = 0; i < 100; ++i) {
        many += "line\n";
    }
    const QString trimmed = sanitizeRemoteText(many, 4096, 3);
    CHECK(trimmed.count(u'\n') == 2);
    CHECK(trimmed.endsWith(QChar(0x2026)));
}

TEST_CASE("a huge payload is bounded before it is decoded", "[net][remote]") {
    // Ten megabytes of prompt should cost a truncation, not ten megabytes of
    // QString on the way to being shortened.
    const std::string huge(10 * 1024 * 1024, 'z');
    const QString out = sanitizeRemoteText(huge, 64, 12);
    CHECK(out.size() == 65);
}

TEST_CASE("invalid UTF-8 becomes visible, not invisible", "[net][remote]") {
    // Dropping bad bytes silently would let a server show the user one thing
    // while knowing it sent another.
    const QString out = sanitizeRemoteText(std::string("ok\xff\xfe"
                                                       "end",
                                                       8));
    CHECK(out.startsWith(QStringLiteral("ok")));
    CHECK(out.endsWith(QStringLiteral("end")));
    CHECK(out.contains(QChar(0xFFFD)));
}

TEST_CASE("text that needs no clipping gets no ellipsis", "[net][remote]") {
    CHECK_FALSE(sanitizeRemoteText("short").endsWith(QChar(0x2026)));
    // Exactly at the cap is not clipped.
    CHECK(sanitizeRemoteText("abcd", 4, 12) == QStringLiteral("abcd"));
}
