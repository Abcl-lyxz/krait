#include "actions.h"

#include <algorithm>
#include <array>

namespace krait::app::session {

namespace {

// The M2 action set. Keywords carry the words people reach for that are NOT in
// the label — "quit" for closing, "find" for search — because a palette that
// only matches its own labels is a palette you have to already know.
constexpr std::array kActions = {
    Action{"session.new", "New session", "Ctrl+Shift+T", "open tab create"},
    Action{"session.close", "Close session", "Ctrl+Shift+W", "quit exit tab"},
    Action{"session.reconnect", "Reconnect session", "", "retry reopen"},
    // T53. Ctrl+9 is "last tab" rather than "tab 9", the convention every
    // browser uses; the numbered jumps are bound in Main.qml because nine of
    // them do not each deserve a registry row.
    Action{"session.next", "Next tab", "Ctrl+Tab", "switch forward right"},
    Action{"session.previous", "Previous tab", "Ctrl+Shift+Tab", "switch back left"},
    Action{"view.splitRight", "Split right", "Ctrl+Shift+D", "pane divide side vertical"},
    Action{"view.splitDown", "Split down", "Ctrl+Shift+E", "pane divide below horizontal"},
    Action{"view.closePane", "Close pane", "", "split remove"},
    Action{"palette.open", "Command palette", "Ctrl+Shift+P", "commands run action"},
    Action{"sessions.open", "Open a saved session", "Ctrl+Shift+O", "connect profile ssh host"},
    Action{"sessions.manage", "Manage sessions", "", "profiles edit folders tree"},
    Action{"sessions.import.putty", "Import sessions from PuTTY", "", "migrate registry"},
    Action{"settings.open", "Settings", "Ctrl+,", "preferences options config"},
    Action{"settings.reload", "Reload settings from disk", "", "refresh config"},
    Action{"edit.copy", "Copy", "Ctrl+Shift+C", "clipboard selection"},
    Action{"edit.paste", "Paste", "Ctrl+Shift+V", "clipboard insert"},
    Action{"edit.search", "Search scrollback", "Ctrl+Shift+F", "find grep regex history"},
    Action{"view.clearScrollback", "Clear scrollback", "", "history erase"},
    Action{"help.about", "About Krait", "", "version"},
};

}  // namespace

std::span<const Action> allActions() {
    return kActions;
}

const Action* findAction(std::string_view id) {
    const auto it = std::find_if(kActions.begin(), kActions.end(),
                                 [id](const Action& action) { return action.id == id; });
    return it == kActions.end() ? nullptr : &*it;
}

}  // namespace krait::app::session
