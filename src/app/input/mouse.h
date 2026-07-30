#pragma once

#include "core/grid/grid.h"

#include <QByteArray>
#include <Qt>

#include <cstdint>

namespace krait::app::input {

enum class MouseAction : std::uint8_t { Press, Release, Move };

// One mouse event in TERMINAL coordinates: row and col are 0-based cells, not
// pixels. Turning pixels into cells is the item's job — it owns the metrics and
// the device pixel ratio — and this layer only encodes.
struct MouseEvent {
    MouseAction action = MouseAction::Press;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttonsDown = Qt::NoButton;  // which are held during a move
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    int row = 0;
    int col = 0;
    int wheelSteps = 0;  // +up / -down; non-zero means this is a wheel event
};

// Encodes a mouse event for the application, honouring the grid's tracking mode
// (1000/1002/1003) and encoding (1006). Returns empty when the event should not
// be reported — which is most events, most of the time.
//
// Empty is also the signal that the event is OURS to act on: with no tracking a
// drag is a text selection and a wheel scrolls the viewport.
QByteArray encodeMouse(const MouseEvent& event, const core::vt::Grid& grid);

}  // namespace krait::app::input
