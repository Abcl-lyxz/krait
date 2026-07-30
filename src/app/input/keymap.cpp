#include "keymap.h"

namespace krait::app::input {
namespace {

// CSI <n> ~ — the "tilde" family: Insert, Delete, Page Up/Down, F5 and up.
QByteArray tilde(int code, int modParam) {
    if (modParam > 1) {
        return QByteArray("\x1B[") + QByteArray::number(code) + ';' + QByteArray::number(modParam) +
               '~';
    }
    return QByteArray("\x1B[") + QByteArray::number(code) + '~';
}

// The cursor/editing family. Unmodified it is CSI <final>, or SS3 <final> when
// DECCKM is on; modified it is ALWAYS CSI 1 ; <mod> <final> and never SS3 —
// xterm drops application mode the moment a modifier is involved, and a client
// handed SS3 1 ; 5 A cannot parse it.
QByteArray cursor(char final, int modParam, bool appCursorKeys) {
    if (modParam > 1) {
        return QByteArray("\x1B[1;") + QByteArray::number(modParam) + final;
    }
    return QByteArray(appCursorKeys ? "\x1BO" : "\x1B[") + final;
}

// F1-F4 are SS3 P/Q/R/S unmodified and CSI 1 ; <mod> P/Q/R/S modified — the
// same rule as the cursor keys, for the same reason.
QByteArray funcSs3(char final, int modParam) {
    if (modParam > 1) {
        return QByteArray("\x1B[1;") + QByteArray::number(modParam) + final;
    }
    return QByteArray("\x1BO") + final;
}

}  // namespace

int modifierParam(Qt::KeyboardModifiers mods) {
    int param = 1;
    if (mods.testFlag(Qt::ShiftModifier)) {
        param += 1;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        param += 2;
    }
    if (mods.testFlag(Qt::ControlModifier)) {
        param += 4;
    }
    return param;
}

QByteArray translateKey(int key, Qt::KeyboardModifiers mods, const QString& text,
                        const KeyModes& modes) {
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);
    const int mod = modifierParam(mods);

    switch (key) {
    case Qt::Key_Up:
        return cursor('A', mod, modes.appCursorKeys);
    case Qt::Key_Down:
        return cursor('B', mod, modes.appCursorKeys);
    case Qt::Key_Right:
        return cursor('C', mod, modes.appCursorKeys);
    case Qt::Key_Left:
        return cursor('D', mod, modes.appCursorKeys);
    case Qt::Key_Home:
        return cursor('H', mod, modes.appCursorKeys);
    case Qt::Key_End:
        return cursor('F', mod, modes.appCursorKeys);

    case Qt::Key_Insert:
        return tilde(2, mod);
    case Qt::Key_Delete:
        return tilde(3, mod);
    case Qt::Key_PageUp:
        return tilde(5, mod);
    case Qt::Key_PageDown:
        return tilde(6, mod);

    case Qt::Key_F1:
        return funcSs3('P', mod);
    case Qt::Key_F2:
        return funcSs3('Q', mod);
    case Qt::Key_F3:
        return funcSs3('R', mod);
    case Qt::Key_F4:
        return funcSs3('S', mod);
    // The gaps are xterm's, not a typo: codes 16 and 22 are unassigned.
    case Qt::Key_F5:
        return tilde(15, mod);
    case Qt::Key_F6:
        return tilde(17, mod);
    case Qt::Key_F7:
        return tilde(18, mod);
    case Qt::Key_F8:
        return tilde(19, mod);
    case Qt::Key_F9:
        return tilde(20, mod);
    case Qt::Key_F10:
        return tilde(21, mod);
    case Qt::Key_F11:
        return tilde(23, mod);
    case Qt::Key_F12:
        return tilde(24, mod);

    case Qt::Key_Return:
    case Qt::Key_Enter:
        return alt ? QByteArray("\x1B\r") : QByteArray("\r");

    case Qt::Key_Tab:
        return alt ? QByteArray("\x1B\t") : QByteArray("\t");
    case Qt::Key_Backtab:
        return {"\x1B[Z"};  // Shift+Tab

    case Qt::Key_Escape:
        return {"\x1B"};

    case Qt::Key_Backspace:
        // DEL by default, BS under Ctrl. This pair is the single most common
        // "my backspace prints ^H" complaint, and this is the mapping xterm
        // ships with backarrowKey false.
        if (ctrl) {
            return alt ? QByteArray("\x1B\b") : QByteArray("\b");
        }
        return alt ? QByteArray("\x1B\x7F") : QByteArray("\x7F");

    case Qt::Key_Space:
        // Ctrl+Space is NUL, which is how applications read C-@ / set-mark.
        // QKeyEvent::text() is " " here, so without this case it sends a plain
        // space and every Emacs user files the same bug.
        if (ctrl) {
            return alt ? QByteArray("\x1B\0", 2) : QByteArray("\0", 1);
        }
        break;

    // Bare modifiers produce nothing. Listed explicitly so they leave here
    // rather than reaching the text branch, where an empty text() would make
    // them indistinguishable from a key we failed to handle.
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
        return {};

    default:
        break;
    }

    if (ctrl) {
        // C0 from the KEY, not from text(): on a non-US layout text() for
        // Ctrl+<letter> is often empty or the wrong character.
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            const char code = static_cast<char>(key - Qt::Key_A + 1);
            return alt ? QByteArray("\x1B") + code : QByteArray(1, code);
        }
        // The remaining C0 codes. Key_6 and Key_Minus are here because
        // Ctrl+Shift+6 and Ctrl+Shift+- are how ^^ and ^_ are really typed on
        // a US layout — the shifted key never reaches us as ^ or _.
        char code = 0;
        switch (key) {
        case Qt::Key_BracketLeft:
            code = 0x1B;
            break;
        case Qt::Key_Backslash:
            code = 0x1C;
            break;
        case Qt::Key_BracketRight:
            code = 0x1D;
            break;
        case Qt::Key_AsciiCircum:
        case Qt::Key_6:
            code = 0x1E;
            break;
        case Qt::Key_Underscore:
        case Qt::Key_Minus:
            code = 0x1F;
            break;
        default:
            break;
        }
        if (code != 0) {
            return alt ? QByteArray("\x1B") + code : QByteArray(1, code);
        }
    }

    if (text.isEmpty()) {
        return {};  // nothing to send; the caller must not accept the event
    }
    const QByteArray utf8 = text.toUtf8();
    // metaSendsEscape, xterm's default: Alt+x is ESC x, not a high-bit byte.
    // The 8-bit alternative mangles UTF-8 and is off in every modern terminal.
    return alt ? QByteArray("\x1B") + utf8 : utf8;
}

}  // namespace krait::app::input
