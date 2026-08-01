#pragma once

#include "core/grid/cluster_pool.h"
#include "core/grid/line.h"

#include <Qt>

#include <cstdint>
#include <span>

namespace krait::app::input {

// Vim-keys copy mode (plan T71): read the scrollback and take text out of it
// without touching the mouse.
//
// COORDINATES. Everything here is in VIEWPORT rows and grid columns — the same
// space render::Selection uses. That is deliberate: a yank then hands the
// existing copy path exactly what a mouse drag would, so line-wrap joining and
// trailing-space trimming stay one implementation instead of two that disagree.
// The cost is that a scroll re-anchors the selection, which is already true of
// mouse selection and is the reason a motion returns its scroll delta to the
// caller rather than scrolling the grid itself.
//
// WHY THIS IS A SEPARATE, Qt-LIGHT FILE. The interesting part is where a motion
// LANDS, and that is decidable from a span of Lines and a ClusterPool. Keeping
// it out of TerminalItem is what makes the grapheme-cluster and East-Asian-width
// cases testable without a window, a pty or a GPU.

enum class Selecting : std::uint8_t { Off, Char, Line };

enum class Motion : std::uint8_t {
    Left,          // h
    Down,          // j
    Up,            // k
    Right,         // l
    WordNext,      // w
    WordPrev,      // b
    WordEnd,       // e
    LineStart,     // 0
    LineEnd,       // $
    Top,           // g — the oldest scrollback we still hold
    Bottom,        // G — the live screen
    HalfPageUp,    // Ctrl-U
    HalfPageDown,  // Ctrl-D
};

// What one key press asked copy mode to do.
struct Command {
    enum class Kind : std::uint8_t {
        None,  // not ours; the caller must leave the event unaccepted
        Move,
        Select,  // v / V — `select` says which, Off toggles it back off
        Yank,    // y
        Leave,   // Escape
    };
    Kind kind = Kind::None;
    Motion motion = Motion::Left;
    Selecting select = Selecting::Off;
};

// NOT IMPLEMENTED, deliberately: `/` and `n`. searchScrollback() is right there
// and has no consumer yet, but a search needs somewhere to TYPE the pattern,
// and that is a prompt surface — a second thing on screen that eats keys —
// rather than a motion. Binding `n` with no way to set a pattern would ship two
// keys that do nothing, which is worse than not binding them.

// Qt key press to copy-mode command. Returns Kind::None for anything copy mode
// does not claim, so the caller can fall through to the chrome — a mode that
// swallowed every key would take Ctrl+Shift+P with it.
Command translateCopyKey(int key, Qt::KeyboardModifiers mods);

// The copy-mode cursor, and the anchor a selection grows from.
struct CopyCursor {
    int row = 0;
    int col = 0;
    int anchorRow = 0;
    int anchorCol = 0;
    Selecting select = Selecting::Off;
};

// Applies one motion inside `viewport` — the same rows the frame is built from.
//
// Returns how many rows the VIEWPORT must scroll for the cursor to stay on
// screen, positive meaning back into history, which is Grid::scrollView's sign.
// The caller owns the grid and does the scrolling; this function only decides.
//
// INVARIANT, and the reason this file exists: the cursor always comes to rest on
// a grapheme cluster's FIRST cell. It is never left on the right-hand half of a
// double-width cluster and never inside a multi-codepoint one, whichever motion
// put it there — including a vertical motion that changed rows underneath it.
int applyMotion(CopyCursor& cursor, Motion motion, std::span<const core::vt::Line> viewport,
                const core::vt::ClusterPool& clusters);

}  // namespace krait::app::input
