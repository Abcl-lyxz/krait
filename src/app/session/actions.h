#pragma once

#include <span>
#include <string_view>

namespace krait::app::session {

// rules/ui.md: "every action is a registered Action (id, default shortcut,
// palette entry, doc key). A feature reachable only by mouse is incomplete
// work." This is that registry.
//
// One declaration per action, in one place, for the same reason the settings
// schema works that way: the palette, the shortcut table and the docs all read
// from here, so they cannot drift into disagreeing about what a command is
// called or how to reach it.
struct Action {
    // Dotted and stable. It is what a keybinding file names and what a doc
    // anchor uses, so it outlives any label.
    std::string_view id;
    // English label. The palette translates it at display time — the registry
    // stays Qt-free so it can be ranked and tested without a QApplication.
    std::string_view label;
    // Portable spelling ("Ctrl+Shift+P"). Empty means no default binding: an
    // action may be palette-only, but never mouse-only.
    std::string_view shortcut;
    // What the palette matches besides the label: the words someone would
    // actually type. "quit" should find "Close tab".
    std::string_view keywords;
};

// Every action Krait has. Adding one here is what makes it reachable from the
// keyboard and findable in the palette.
std::span<const Action> allActions();

const Action* findAction(std::string_view id);

}  // namespace krait::app::session
