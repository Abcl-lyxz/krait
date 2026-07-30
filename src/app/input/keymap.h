#pragma once

#include <QByteArray>
#include <QString>
#include <Qt>

namespace krait::app::input {

// The terminal state that changes what a key SENDS. Passed in rather than read
// from the grid so translation stays pure: a keymap table test is worth having
// and it must not need a Session, a backend or a window.
struct KeyModes {
    bool appCursorKeys = false;  // DECCKM (mode 1)
};

// Translates one key press into the bytes a terminal sends.
//
// Returns empty when the key produces nothing — a bare modifier, or a key we do
// not claim. The caller MUST then leave the event unaccepted rather than
// swallow it, or shortcuts stop reaching the QML chrome.
//
// `text` is QKeyEvent::text(): already composed by the IME, so Thai and CJK
// arrive here as finished text and need no special case.
//
// NOT implemented, deliberately: win32-input-mode (DECSET 9001). ConPTY speaks
// it, and it is the only way to pass key-up events through and to tell left
// from right modifiers — but claiming it means answering for every key event
// including releases, and an application that enables it and then receives
// ordinary VT input misreads every keystroke. Honest silence until it is real.
QByteArray translateKey(int key, Qt::KeyboardModifiers mods, const QString& text,
                        const KeyModes& modes);

// The xterm modifier parameter: 1 + shift(1) + alt(2) + ctrl(4). Exposed
// because the sequence builders and their tests both need it, and getting the
// bit order wrong produces plausible-looking sequences that are wrong.
int modifierParam(Qt::KeyboardModifiers mods);

}  // namespace krait::app::input
