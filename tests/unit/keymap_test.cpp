// T27 — the keymap table the plan asks to verify, plus the mouse encoders.
//
// A table, not prose: every case here is a byte sequence a real application
// parses, and a plausible-looking wrong answer (SS3 under a modifier, an X10
// release that names its button) stays invisible until something downstream
// misbehaves in a way nobody traces back to here.

#include "app/input/keymap.h"
#include "app/input/mouse.h"
#include <catch2/catch_test_macros.hpp>

using krait::app::input::encodeMouse;
using krait::app::input::KeyModes;
using krait::app::input::modifierParam;
using krait::app::input::MouseAction;
using krait::app::input::MouseEvent;
using krait::app::input::translateKey;
using krait::core::vt::Grid;

namespace {

QByteArray key(int k, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {},
               bool appCursor = false) {
    return translateKey(k, mods, text, KeyModes{.appCursorKeys = appCursor});
}

}  // namespace

TEST_CASE("the modifier parameter follows xterm's bit order", "[input][keymap]") {
    // 1 + shift(1) + alt(2) + ctrl(4). Swapping any two of these compiles and
    // produces sequences that look right until an application disagrees.
    CHECK(modifierParam(Qt::NoModifier) == 1);
    CHECK(modifierParam(Qt::ShiftModifier) == 2);
    CHECK(modifierParam(Qt::AltModifier) == 3);
    CHECK(modifierParam(Qt::ShiftModifier | Qt::AltModifier) == 4);
    CHECK(modifierParam(Qt::ControlModifier) == 5);
    CHECK(modifierParam(Qt::ControlModifier | Qt::ShiftModifier) == 6);
    CHECK(modifierParam(Qt::ControlModifier | Qt::AltModifier) == 7);
    CHECK(modifierParam(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier) == 8);
}

TEST_CASE("cursor keys follow DECCKM, and a modifier cancels it", "[input][keymap]") {
    CHECK(key(Qt::Key_Up) == "\x1B[A");
    CHECK(key(Qt::Key_Down) == "\x1B[B");
    CHECK(key(Qt::Key_Right) == "\x1B[C");
    CHECK(key(Qt::Key_Left) == "\x1B[D");

    // DECCKM on: SS3, which is what readline and vim negotiate for.
    CHECK(key(Qt::Key_Up, Qt::NoModifier, {}, true) == "\x1BOA");
    CHECK(key(Qt::Key_Left, Qt::NoModifier, {}, true) == "\x1BOD");

    // ...but a modifier drops back to CSI even with DECCKM on. SS3 has no
    // parameter slot, so "SS3 1;5 A" is unparseable — this is the case a naive
    // implementation gets wrong, and it surfaces only as Ctrl+arrow silently
    // doing nothing inside an editor.
    CHECK(key(Qt::Key_Right, Qt::ControlModifier) == "\x1B[1;5C");
    CHECK(key(Qt::Key_Right, Qt::ControlModifier, {}, true) == "\x1B[1;5C");
    CHECK(key(Qt::Key_Up, Qt::ShiftModifier) == "\x1B[1;2A");
}

TEST_CASE("home and end are CSI H and CSI F", "[input][keymap]") {
    CHECK(key(Qt::Key_Home) == "\x1B[H");
    CHECK(key(Qt::Key_End) == "\x1B[F");
    CHECK(key(Qt::Key_Home, Qt::NoModifier, {}, true) == "\x1BOH");
    CHECK(key(Qt::Key_End, Qt::ControlModifier) == "\x1B[1;5F");
}

TEST_CASE("the tilde family carries xterm's numbering, gaps included", "[input][keymap]") {
    CHECK(key(Qt::Key_Insert) == "\x1B[2~");
    CHECK(key(Qt::Key_Delete) == "\x1B[3~");
    CHECK(key(Qt::Key_PageUp) == "\x1B[5~");
    CHECK(key(Qt::Key_PageDown) == "\x1B[6~");
    CHECK(key(Qt::Key_Delete, Qt::ControlModifier) == "\x1B[3;5~");

    // F1-F4 are SS3; F5+ are tilde codes with 16 and 22 skipped. Those two
    // gaps are the detail everyone re-derives wrongly from a table.
    CHECK(key(Qt::Key_F1) == "\x1BOP");
    CHECK(key(Qt::Key_F4) == "\x1BOS");
    CHECK(key(Qt::Key_F5) == "\x1B[15~");
    CHECK(key(Qt::Key_F6) == "\x1B[17~");
    CHECK(key(Qt::Key_F10) == "\x1B[21~");
    CHECK(key(Qt::Key_F11) == "\x1B[23~");
    CHECK(key(Qt::Key_F12) == "\x1B[24~");
    CHECK(key(Qt::Key_F1, Qt::ShiftModifier) == "\x1B[1;2P");
}

TEST_CASE("control keys produce the right C0 codes", "[input][keymap]") {
    CHECK(key(Qt::Key_A, Qt::ControlModifier, "a") == QByteArray("\x01"));
    CHECK(key(Qt::Key_C, Qt::ControlModifier, "c") == QByteArray("\x03"));
    CHECK(key(Qt::Key_Z, Qt::ControlModifier, "z") == QByteArray("\x1A"));
    // Ctrl+Space is NUL — an embedded zero, so the SIZE must be asserted or the
    // comparison passes against an empty QByteArray and proves nothing.
    const QByteArray nul = key(Qt::Key_Space, Qt::ControlModifier, " ");
    REQUIRE(nul.size() == 1);
    CHECK(nul.at(0) == '\0');

    CHECK(key(Qt::Key_BracketLeft, Qt::ControlModifier, "[") == QByteArray("\x1B"));
    CHECK(key(Qt::Key_Backslash, Qt::ControlModifier, "\\") == QByteArray("\x1C"));
    CHECK(key(Qt::Key_BracketRight, Qt::ControlModifier, "]") == QByteArray("\x1D"));
    // ^^ and ^_ as they are actually typed on a US layout.
    CHECK(key(Qt::Key_6, Qt::ControlModifier | Qt::ShiftModifier, "^") == QByteArray("\x1E"));
    CHECK(key(Qt::Key_Minus, Qt::ControlModifier, "-") == QByteArray("\x1F"));
}

TEST_CASE("backspace sends DEL, and BS only under control", "[input][keymap]") {
    CHECK(key(Qt::Key_Backspace) == QByteArray("\x7F"));
    CHECK(key(Qt::Key_Backspace, Qt::ControlModifier) == QByteArray("\b"));
    CHECK(key(Qt::Key_Backspace, Qt::AltModifier) == QByteArray("\x1B\x7F"));
}

TEST_CASE("alt is a meta ESC prefix, not a high bit", "[input][keymap]") {
    // metaSendsEscape. The 8-bit alternative sets bit 7, which mangles UTF-8;
    // it is off in every modern terminal and must not be the default here.
    CHECK(key(Qt::Key_X, Qt::AltModifier, "x") == QByteArray("\x1Bx"));
    CHECK(key(Qt::Key_Return, Qt::AltModifier) == QByteArray("\x1B\r"));
    CHECK(key(Qt::Key_A, Qt::ControlModifier | Qt::AltModifier, "a") == QByteArray("\x1B\x01"));
}

TEST_CASE("composed text passes through as UTF-8", "[input][keymap]") {
    // What an IME hands over is finished text and nothing here may special-case
    // it. Thai is the first-class locale, so Thai is what gets asserted.
    CHECK(key(Qt::Key_unknown, Qt::NoModifier, QString::fromUtf8("ก")) ==
          QByteArray::fromHex("e0b881"));
    CHECK(key(Qt::Key_unknown, Qt::NoModifier, QString::fromUtf8("สวัสดี")) ==
          QString::fromUtf8("สวัสดี").toUtf8());
}

TEST_CASE("keys that send nothing return empty", "[input][keymap]") {
    // The caller has to tell "handled, sent nothing" from "not mine": empty
    // means leave the event unaccepted so the chrome still sees it.
    CHECK(key(Qt::Key_Shift).isEmpty());
    CHECK(key(Qt::Key_Control).isEmpty());
    CHECK(key(Qt::Key_CapsLock).isEmpty());
    CHECK(key(Qt::Key_F13).isEmpty());  // beyond what we claim
    CHECK(key(Qt::Key_unknown).isEmpty());
}

TEST_CASE("no mouse report is sent while tracking is off", "[input][mouse]") {
    Grid grid(24, 80);
    // Off is the default and the common case: with tracking off a drag is a
    // text selection, and reporting it would break selection everywhere.
    CHECK(encodeMouse(MouseEvent{.button = Qt::LeftButton, .row = 3, .col = 5}, grid).isEmpty());
}

TEST_CASE("SGR encoding names the button on release", "[input][mouse]") {
    Grid grid(24, 80);
    grid.mouseTracking = Grid::MouseTracking::Normal;
    grid.sgrMouse = true;

    CHECK(encodeMouse(MouseEvent{.button = Qt::LeftButton, .row = 3, .col = 5}, grid) ==
          "\x1B[<0;6;4M");
    CHECK(encodeMouse(
              MouseEvent{
                  .action = MouseAction::Release, .button = Qt::RightButton, .row = 3, .col = 5},
              grid) == "\x1B[<2;6;4m");
    // Modifiers ride in the same field: shift 4, alt 8, ctrl 16.
    CHECK(encodeMouse(
              MouseEvent{.button = Qt::LeftButton, .mods = Qt::ControlModifier, .row = 0, .col = 0},
              grid) == "\x1B[<16;1;1M");
}

TEST_CASE("X10 encoding drops a report it cannot express", "[input][mouse]") {
    Grid grid(24, 400);
    grid.mouseTracking = Grid::MouseTracking::Normal;

    CHECK(encodeMouse(MouseEvent{.button = Qt::LeftButton, .row = 0, .col = 0}, grid) ==
          QByteArray("\x1B[M\x20\x21\x21"));
    // Past column 223 a coordinate does not fit in a byte. Silence is
    // recoverable; a wrapped byte is a click reported in the wrong cell.
    CHECK(encodeMouse(MouseEvent{.button = Qt::LeftButton, .row = 0, .col = 300}, grid).isEmpty());
    // ...and with SGR on the same event reports fine. This pair is 1006's
    // entire reason to exist.
    grid.sgrMouse = true;
    CHECK(encodeMouse(MouseEvent{.button = Qt::LeftButton, .row = 0, .col = 300}, grid) ==
          "\x1B[<0;301;1M");
}

TEST_CASE("motion is reported only when the mode asks for it", "[input][mouse]") {
    Grid grid(24, 80);
    grid.sgrMouse = true;
    const MouseEvent drag{
        .action = MouseAction::Move, .buttonsDown = Qt::LeftButton, .row = 2, .col = 2};
    const MouseEvent hover{.action = MouseAction::Move, .row = 2, .col = 2};

    grid.mouseTracking = Grid::MouseTracking::Normal;
    CHECK(encodeMouse(drag, grid).isEmpty());
    CHECK(encodeMouse(hover, grid).isEmpty());

    grid.mouseTracking = Grid::MouseTracking::ButtonEvent;
    CHECK(encodeMouse(drag, grid) == "\x1B[<32;3;3M");  // 32 = motion + button 0
    CHECK(encodeMouse(hover, grid).isEmpty());

    grid.mouseTracking = Grid::MouseTracking::AnyEvent;
    CHECK(encodeMouse(drag, grid) == "\x1B[<32;3;3M");
    CHECK(encodeMouse(hover, grid) == "\x1B[<35;3;3M");  // 32 + 3 = no button
}

TEST_CASE("the wheel reports as buttons 4 and 5 with no release", "[input][mouse]") {
    Grid grid(24, 80);
    grid.mouseTracking = Grid::MouseTracking::Normal;
    grid.sgrMouse = true;
    CHECK(encodeMouse(MouseEvent{.row = 1, .col = 1, .wheelSteps = 1}, grid) == "\x1B[<64;2;2M");
    CHECK(encodeMouse(MouseEvent{.row = 1, .col = 1, .wheelSteps = -1}, grid) == "\x1B[<65;2;2M");
}
