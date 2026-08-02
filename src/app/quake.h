#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>

#include <optional>

class QVariantAnimation;
class QWindow;

namespace krait::app {

namespace settings {
class Registry;
}

// Quake mode: a drop-down terminal on a system-wide hotkey (plan T74).
//
// Two halves, split the way taskbar_progress.h splits: the DECISIONS below are
// free functions with no window and no OS behind them, so the hotkey spelling
// and the multi-monitor arithmetic are testable; the class underneath is the
// thin part that talks to Win32 and cannot be tested in CI at all.

// A hotkey as RegisterHotKey wants it. Plain unsigned ints rather than the
// Win32 typedefs so this header does not drag <windows.h> into everything that
// includes it — the values ARE MOD_* and VK_*, set in quake.cpp.
struct Hotkey {
    unsigned int modifiers = 0;
    unsigned int key = 0;

    bool operator==(const Hotkey& other) const = default;
};

// Parses Krait's ordinary shortcut spelling ("Ctrl+Alt+`", "Ctrl+Shift+F12")
// into what RegisterHotKey takes. Empty on anything it cannot express, because
// a global hotkey that half-works is worse than one that says it is off.
//
// MOD_NOREPEAT is always added: without it holding the key down toggles the
// window at the keyboard's auto-repeat rate.
//
// One rule that is not just parsing: a combination with NO modifier is refused
// unless the key is a function key. A bare letter registered system-wide takes
// that letter away from every other program on the machine, and nothing about
// the settings UI would explain what had happened.
std::optional<Hotkey> parseHotkey(const QString& text);

// Where the drop-down sits on a screen whose usable area is `available`.
//
// Everything here is in DEVICE-INDEPENDENT pixels, in and out: QScreen
// geometry and QWindow::setGeometry both are (doc.qt.io "High DPI" — "Qt
// applications generally operate in device-independent pixels. This includes
// window and screen geometries reported to the application"), so there is no
// DPI arithmetic to get wrong and none is done. The repo's DPI handling stays
// where it already is, in the renderer's devicePixelRatio path.
//
// `available.x()` and `.y()` are used rather than assumed zero: a monitor
// placed left of or above the primary one has NEGATIVE coordinates, and that
// is the multi-monitor bug this function exists to not have.
QRect quakeGeometry(const QRect& available, int heightPercent);

// The OS half. Owned by main() rather than a singleton for the same reason
// TaskbarProgress is: it installs a native event filter and holds a system-wide
// registration, and both must be released while QGuiApplication is still alive.
class QuakeWindow : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

  public:
    explicit QuakeWindow(QObject* parent = nullptr);  // owned by parent
    ~QuakeWindow() override;

    // Turns `window` into a drop-down and claims the hotkey.
    //
    // FALSE means the hotkey could not be registered — almost always because
    // another program already owns it, which is the common real failure. The
    // caller must then leave the window VISIBLE: a hidden window whose only way
    // back is a hotkey that does nothing is a trap. What to SAY about it comes
    // through hotkeyFailed, so the startup case and a later live edit of
    // quake.hotkey reach the same banner by the same route.
    //
    // True with an empty quake.hotkey means quake mode is off and the window
    // was left exactly as it was.
    bool attach(QWindow* window, settings::Registry* registry);

    // Whether the window was actually turned into a drop-down, i.e. whether
    // quake.hotkey named something. Decides whether main() starts it hidden.
    bool dropDown() const { return m_dropDown; }

    // WM_HOTKEY. Both Windows eventType strings are accepted; see the note in
    // the implementation for why one is not enough.
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

    // Drops the window in or takes it away. Public so the hotkey is not the
    // only way to reach it from a test build or a future palette entry.
    void toggle();

  signals:
    // The hotkey could not be claimed. Emitted from attach() AND from every
    // live re-registration after quake.hotkey changes — the second is the one
    // that would otherwise be silent, and it is the more likely of the two,
    // since editing the setting is exactly what someone does after being told
    // the first combination was taken.
    void hotkeyFailed(const QString& message, const QString& detail);

  private:
    void showDropDown();
    void hideDropDown();
    // Re-reads quake.hotkey and re-registers. Live, because a failed
    // registration must be fixable without restarting the app that told you
    // about it.
    void applyHotkey();
    void unregisterHotkey();
    // Makes the window reachable again, then says what went wrong.
    void reportHotkeyFailure();
    int heightPercent() const;

    QPointer<QWindow> m_window;
    settings::Registry* m_settings = nullptr;  // borrowed; owned by main()
    QVariantAnimation* m_slide = nullptr;      // owned by this (QObject parent)
    // HWNDs, as void* so this header stays free of <windows.h>.
    //
    // m_hwnd is the window the hotkey was REGISTERED against, cached at
    // registration. Not re-read from winId() at unregister time, and that is
    // not tidiness: winId() *creates* the platform window, so calling it during
    // teardown — after ~QQuickWindow has destroyed the native half — mints a
    // fresh HWND and then unregisters a hotkey on a window that never had one,
    // leaving the real registration alive until the process exits. Notifier
    // caches its HWND for the same reason.
    void* m_hwnd = nullptr;
    // What had the keyboard before the drop-down took it, so hiding can give it
    // back — the half of "toggle" people only notice when it is missing.
    void* m_previousForeground = nullptr;
    QString m_error;
    QString m_errorHint;
    bool m_dropDown = false;
    bool m_registered = false;
    bool m_visible = false;
};

}  // namespace krait::app
