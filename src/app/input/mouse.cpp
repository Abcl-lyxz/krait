#include "mouse.h"

namespace krait::app::input {
namespace {

using Tracking = core::vt::Grid::MouseTracking;

// The low two bits of the button field. 3 means "no button", which in X10
// encoding is also how a release is reported — that ambiguity, not the
// coordinate limit, is the first reason 1006 exists.
constexpr int kButtonLeft = 0;
constexpr int kButtonMiddle = 1;
constexpr int kButtonRight = 2;
constexpr int kButtonNone = 3;

constexpr int kShiftBit = 4;
constexpr int kMetaBit = 8;
constexpr int kCtrlBit = 16;
constexpr int kMotionBit = 32;
constexpr int kWheelBit = 64;

int buttonCode(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton:
        return kButtonLeft;
    case Qt::MiddleButton:
        return kButtonMiddle;
    case Qt::RightButton:
        return kButtonRight;
    default:
        return kButtonNone;
    }
}

// The button still held during a drag. With several down the lowest wins,
// which is what every implementation does.
int heldButtonCode(Qt::MouseButtons buttons) {
    if (buttons.testFlag(Qt::LeftButton)) {
        return kButtonLeft;
    }
    if (buttons.testFlag(Qt::MiddleButton)) {
        return kButtonMiddle;
    }
    if (buttons.testFlag(Qt::RightButton)) {
        return kButtonRight;
    }
    return kButtonNone;
}

int modifierBits(Qt::KeyboardModifiers mods) {
    int bits = 0;
    if (mods.testFlag(Qt::ShiftModifier)) {
        bits |= kShiftBit;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        bits |= kMetaBit;
    }
    if (mods.testFlag(Qt::ControlModifier)) {
        bits |= kCtrlBit;
    }
    return bits;
}

}  // namespace

QByteArray encodeMouse(const MouseEvent& event, const core::vt::Grid& grid) {
    if (grid.mouseTracking == Tracking::Off) {
        return {};
    }

    int code = 0;
    bool release = false;

    if (event.wheelSteps != 0) {
        // Wheel is buttons 4/5 with bit 64 set, and it has NO release event —
        // sending one makes applications scroll twice per notch.
        code = kWheelBit | (event.wheelSteps > 0 ? 0 : 1);
    } else {
        switch (event.action) {
        case MouseAction::Press:
            code = buttonCode(event.button);
            break;
        case MouseAction::Release:
            code = buttonCode(event.button);
            release = true;
            break;
        case MouseAction::Move: {
            const bool held = event.buttonsDown != Qt::NoButton;
            // 1000 reports no motion at all; 1002 reports it only while a
            // button is down. Getting this wrong floods the pty with reports
            // the application never asked for and cannot keep up with.
            if (grid.mouseTracking == Tracking::Normal) {
                return {};
            }
            if (grid.mouseTracking == Tracking::ButtonEvent && !held) {
                return {};
            }
            code = kMotionBit | (held ? heldButtonCode(event.buttonsDown) : kButtonNone);
            break;
        }
        }
    }

    code |= modifierBits(event.mods);

    if (grid.sgrMouse) {
        // CSI < Cb ; Cx ; Cy M|m — 'm' for release. 1-based, no upper bound,
        // and the button survives the release, which is the point of the mode.
        return QByteArray("\x1B[<") + QByteArray::number(code) + ';' +
               QByteArray::number(event.col + 1) + ';' + QByteArray::number(event.row + 1) +
               (release ? 'm' : 'M');
    }

    // X10: CSI M Cb Cx Cy, every field one byte biased by 32. A release cannot
    // name its button, and a coordinate past 223 does not fit in a byte at all
    // — xterm drops such a report rather than name the wrong cell, and so do
    // we. Silence is recoverable; a wrong cell is a click in the wrong place.
    if (release) {
        code = kButtonNone | modifierBits(event.mods);
    }
    constexpr int kBias = 32;
    constexpr int kMaxCoord = 255 - kBias;  // columns/rows 1..223
    if (event.col + 1 > kMaxCoord || event.row + 1 > kMaxCoord) {
        return {};
    }
    QByteArray out("\x1B[M");
    out += static_cast<char>(code + kBias);
    out += static_cast<char>(event.col + 1 + kBias);
    out += static_cast<char>(event.row + 1 + kBias);
    return out;
}

}  // namespace krait::app::input
