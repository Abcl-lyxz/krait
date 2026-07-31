#pragma once

#include <QString>

#include <cstddef>
#include <string_view>

namespace krait::net {

// Text the REMOTE side chose, on its way to a human.
//
// rules/net.md: all remote input is hostile. A keyboard-interactive prompt, a
// telnet banner and an SSH error string are all attacker-controlled if the
// server is, and they land in a banner next to words the user trusts. Three
// things have to go before that is safe:
//
//  - C0 and C1 controls, so the text cannot move the cursor, start an escape
//    sequence, or overwrite the label that says what it is;
//  - unbounded length, so a server cannot push the rest of the UI off-screen
//    or make the app chew through a 10 MB "prompt";
//  - unbounded line count, for the same reason.
//
// Truncation is marked with a horizontal ellipsis, so a clipped prompt reads as
// clipped rather than as the whole message.
QString sanitizeRemoteText(std::string_view text, std::size_t maxChars = 512, int maxLines = 12);

}  // namespace krait::net
