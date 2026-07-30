#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>

namespace krait::app::input {

// Why a paste needs confirming. Ordered by severity: the highest one found
// wins, because a banner that says "multiline" about a `sudo rm -rf` is worse
// than useless — it points the user at the wrong thing to check.
enum class PasteRisk : std::uint8_t {
    None,
    Multiline,         // more than one line: the shell will run all of them
    ExecutesOnPaste,   // ends in a newline: the shell runs it with no keypress
    DangerousCommand,  // sudo, rm -rf and friends
};

struct PasteResult {
    // The bytes to send, already wrapped in ESC[200~ / ESC[201~ when the
    // application asked for bracketed paste. Safe to send as-is.
    QByteArray bytes;
    PasteRisk risk = PasteRisk::None;
    // Whether the sanitiser removed anything. Worth telling the user: silently
    // altering what they pasted is its own kind of surprise.
    bool sanitised = false;

    bool needsConfirm() const { return risk != PasteRisk::None; }
};

// Sanitises clipboard text and prepares it for the pty.
//
// rules/net.md treats remote input as hostile, and the clipboard IS remote
// input: a web page can put anything on it, including text whose real payload
// is escape sequences. So:
//
//  - every C0 control except tab and newline is dropped, ESC first among them.
//    A pasted ESC sequence drives the terminal — it can set modes, relabel the
//    window, or, on a terminal with a report-back sequence, make the terminal
//    inject its own reply as if it were typed.
//  - the bracketed-paste END marker is neutralised inside the payload. Without
//    that, text containing ESC[201~ closes the bracket early and everything
//    after it arrives as though the user typed it, defeating the whole mode.
//  - CRLF and lone CR become CR, which is what a terminal sends for Enter.
//
// `bracketed` is grid.bracketedPaste (DECSET 2004).
PasteResult preparePaste(const QString& text, bool bracketed);

// A translated, user-facing sentence for a risk. Lives here so the wording and
// the detection cannot drift apart.
QString describeRisk(PasteRisk risk);

}  // namespace krait::app::input
