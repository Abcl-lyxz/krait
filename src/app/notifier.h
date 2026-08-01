#pragma once

#include <QString>

class QWindow;

namespace krait::app {

// A real desktop notification (T68) — the half of M4's "long build over SSH →
// progress in taskbar → notification on finish → jump-to-prompt" demo that T67
// left out.
//
// `Shell_NotifyIconW` with `NIF_INFO`: a shell balloon, which Windows 10 shows
// as a banner and keeps in the notification centre. Windows 11 is DIFFERENT and
// this deliberately does not promise otherwise — learn.microsoft.com says 11
// "more closely follows the legacy behavior in making them transient", so a
// dismissed balloon is gone rather than filed.
//
// Native rather than `QSystemTrayIcon`, which lives in Qt Widgets: reaching the
// same Win32 call through it would mean linking QtWidgets and swapping this
// QGuiApplication-only QML shell for a QApplication — a dependency, a startup
// cost and a second event-loop integration for a call that is thirty lines.
//
// COMPLEMENTARY to the per-tab banner and `QWindow::alert()`, not a replacement
// for either: the banner is the surface that survives being missed, and the
// taskbar flash is what a user still looking at the taskbar sees. This is the
// only one of the three that reaches someone in another window.
//
// No unit test, for the same reason `TaskbarProgress`'s OS half has none: every
// line here is a shell call, and the only decision it makes (the title
// fallback) is one branch. `TaskbarProgress` keeps its rule in a pure
// `aggregateProgress` precisely because that part IS worth testing; there is no
// equivalent here.
class Notifier {
  public:
    Notifier() = default;
    ~Notifier();

    // Owns a shell resource keyed by (HWND, uID) that exactly one destructor
    // may retract, so it neither copies nor moves.
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;
    Notifier(Notifier&&) = delete;
    Notifier& operator=(Notifier&&) = delete;

    // Shows a balloon on `window`'s taskbar icon, adding the icon on first use.
    //
    // The window is passed per call rather than attached once because that is
    // all this needs: the HWND is captured the first time and cached for the
    // NIM_DELETE in the destructor, which has to run after the QML engine —
    // and therefore the window — is already gone.
    //
    // `title` heads the balloon and must not be empty (documented: "if the
    // szInfoTitle member is zero-length, the icon is not shown"), so an empty
    // one falls back to the application name. An empty `body` is not a
    // notification and is dropped.
    void notify(QWindow* window, const QString& title, const QString& body);

  private:
    bool ensureIcon(QWindow* window);

    // The HWND the icon was added to. `void*` so <windows.h> stays out of this
    // header; borrowed, never owned — the QML engine owns the window.
    void* m_hwnd = nullptr;
    bool m_added = false;
};

}  // namespace krait::app
