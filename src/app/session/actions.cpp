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
    // T57. Both are per-SESSION state rather than settings — a hexdump is
    // something you turn on for the device that is misbehaving, not a
    // preference — which is why they live here and not in the registry.
    Action{"view.hexdump", "Toggle hexdump", "Ctrl+Shift+H", "hex bytes raw debug dump"},
    Action{"session.log", "Start or stop logging this session", "", "record capture file"},
    Action{"view.tunnels", "Show port forwards", "Ctrl+Shift+U", "tunnel forward port socks"},
    // T65. "Browse" earns the B; every other file word people reach for is in
    // the keywords, because nobody guesses what a panel is called.
    Action{"view.files", "Show the file transfer panel", "Ctrl+Shift+B",
           "sftp scp files upload download transfer copy browse"},
    // T67. Ctrl+Shift+Up/Down rather than a letter: this is navigation, it is
    // pressed repeatedly, and the arrows are what iTerm2 and VS Code's terminal
    // already trained people to reach for.
    Action{"view.previousPrompt", "Jump to the previous prompt", "Ctrl+Shift+Up",
           "prompt mark shell integration jump back scroll command"},
    Action{"view.nextPrompt", "Jump to the next prompt", "Ctrl+Shift+Down",
           "prompt mark shell integration jump forward scroll command"},
    Action{"palette.open", "Command palette", "Ctrl+Shift+P", "commands run action"},
    Action{"sessions.open", "Open a saved session", "Ctrl+Shift+O", "connect profile ssh host"},
    Action{"sessions.manage", "Manage sessions", "", "profiles edit folders tree"},
    Action{"sessions.import.putty", "Import sessions from PuTTY", "", "migrate registry"},
    // T62. Separate actions rather than one "Import…" that then asks which:
    // the palette is the fastest path to any of them, and a menu behind a menu
    // is the thing the palette exists to replace.
    Action{"sessions.import.sshconfig", "Import hosts from OpenSSH config", "",
           "migrate ssh_config openssh"},
    Action{"sessions.import.mremoteng", "Import connections from mRemoteNG", "",
           "migrate confcons xml"},
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
