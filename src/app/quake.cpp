// WIN32_LEAN_AND_MEAN / NOMINMAX before anything reaches <windows.h>, for the
// same reason taskbar_progress.cpp does it: the min/max macros otherwise land
// in scope for the whole translation unit and break std::clamp below.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "quake.h"

#include "settings/registry.h"
#include <windows.h>

#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QVariantAnimation>
#include <QWindow>

#include <algorithm>

namespace krait::app {

namespace {

// Any value in 0x0000-0xBFFF belongs to the application (a shared DLL would
// have to take one from GlobalAddAtom instead). One hotkey, so one id.
constexpr int kHotkeyId = 1;

// Long enough to read as a movement, short enough that it never feels like
// something to wait for.
constexpr int kSlideMs = 110;

unsigned int modifierBit(const QString& word) {
    // Values from learn.microsoft.com (RegisterHotKey, fsModifiers table).
    if (word.compare(QLatin1String("ctrl"), Qt::CaseInsensitive) == 0 ||
        word.compare(QLatin1String("control"), Qt::CaseInsensitive) == 0) {
        return MOD_CONTROL;
    }
    if (word.compare(QLatin1String("alt"), Qt::CaseInsensitive) == 0) {
        return MOD_ALT;
    }
    if (word.compare(QLatin1String("shift"), Qt::CaseInsensitive) == 0) {
        return MOD_SHIFT;
    }
    if (word.compare(QLatin1String("win"), Qt::CaseInsensitive) == 0 ||
        word.compare(QLatin1String("meta"), Qt::CaseInsensitive) == 0 ||
        word.compare(QLatin1String("super"), Qt::CaseInsensitive) == 0) {
        return MOD_WIN;
    }
    return 0;
}

// The virtual-key code for one key name, and whether it is a function key —
// the only kind allowed to stand alone (see parseHotkey).
struct KeyCode {
    unsigned int vk = 0;
    bool function = false;
};

std::optional<KeyCode> keyCode(const QString& word) {
    if (word.size() == 1) {
        const QChar ch = word.at(0).toUpper();
        // A-Z and 0-9 are their own virtual-key codes; that is the whole
        // reason the ASCII values are used directly rather than mapped.
        if ((ch >= u'A' && ch <= u'Z') || (ch >= u'0' && ch <= u'9')) {
            return KeyCode{.vk = static_cast<unsigned int>(ch.unicode()), .function = false};
        }
        if (ch == u'`') {
            return KeyCode{.vk = VK_OEM_3, .function = false};
        }
        return std::nullopt;
    }
    if ((word.at(0) == u'F' || word.at(0) == u'f') && word.size() <= 3) {
        bool ok = false;
        const int number = QStringView{word}.mid(1).toInt(&ok);
        if (ok && number >= 1 && number <= 24) {
            return KeyCode{.vk = static_cast<unsigned int>(VK_F1 + number - 1), .function = true};
        }
        return std::nullopt;
    }
    if (word.compare(QLatin1String("space"), Qt::CaseInsensitive) == 0) {
        return KeyCode{.vk = VK_SPACE, .function = false};
    }
    if (word.compare(QLatin1String("backtick"), Qt::CaseInsensitive) == 0 ||
        word.compare(QLatin1String("grave"), Qt::CaseInsensitive) == 0) {
        return KeyCode{.vk = VK_OEM_3, .function = false};
    }
    if (word.compare(QLatin1String("esc"), Qt::CaseInsensitive) == 0 ||
        word.compare(QLatin1String("escape"), Qt::CaseInsensitive) == 0) {
        return KeyCode{.vk = VK_ESCAPE, .function = false};
    }
    if (word.compare(QLatin1String("tab"), Qt::CaseInsensitive) == 0) {
        return KeyCode{.vk = VK_TAB, .function = false};
    }
    return std::nullopt;
}

}  // namespace

std::optional<Hotkey> parseHotkey(const QString& text) {
    // ponytail: split on '+', so the '+' KEY itself cannot be bound. Nobody
    // binds a drop-down to plus; upgrade path is a trailing-token special case.
    const QStringList parts = text.split(u'+', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return std::nullopt;
    }

    unsigned int modifiers = 0;
    std::optional<KeyCode> key;
    for (const QString& raw : parts) {
        const QString word = raw.trimmed();
        if (word.isEmpty()) {
            return std::nullopt;
        }
        if (const unsigned int bit = modifierBit(word); bit != 0) {
            modifiers |= bit;
            continue;
        }
        if (key.has_value()) {
            return std::nullopt;  // two keys is not a combination, it is a typo
        }
        key = keyCode(word);
        if (!key.has_value()) {
            return std::nullopt;
        }
    }
    if (!key.has_value()) {
        return std::nullopt;
    }
    // A bare letter or digit registered system-wide takes that key away from
    // every other program running. Function keys are the exception because
    // that is what a drop-down terminal is conventionally bound to and because
    // they are not something anyone types into a document.
    if (modifiers == 0 && !key->function) {
        return std::nullopt;
    }
    // MOD_NOREPEAT (0x4000, Windows 7 and later). Without it, holding the
    // combination down toggles the window at the auto-repeat rate.
    return Hotkey{.modifiers = modifiers | MOD_NOREPEAT, .key = key->vk};
}

QRect quakeGeometry(const QRect& available, int heightPercent) {
    // Clamped here as well as in the schema: this is also reachable from a
    // hand-edited file, and a zero-height window is one nobody can find again.
    const int percent = std::clamp(heightPercent, 10, 100);
    const int height = std::max(1, available.height() * percent / 100);
    return QRect{available.x(), available.y(), available.width(), height};
}

QuakeWindow::QuakeWindow(QObject* parent)
    : QObject(parent), m_slide(new QVariantAnimation(this)) {  // owned by this (QObject parent)
    m_slide->setDuration(kSlideMs);
    m_slide->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        if (m_window.isNull()) {
            return;
        }
        // POSITION only, never size. Animating the height would resize the
        // swapchain and reflow the grid — and therefore the pty — on every
        // frame of the animation, which the far end would see as a burst of
        // SIGWINCH. Moving a fixed-size window costs neither.
        m_window->setPosition(m_window->x(), value.toInt());
    });
}

QuakeWindow::~QuakeWindow() {
    unregisterHotkey();
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
}

bool QuakeWindow::attach(QWindow* window, settings::Registry* registry) {
    m_window = window;
    m_settings = registry;
    if (window == nullptr || registry == nullptr) {
        return true;
    }
    if (QString::fromStdString(registry->text("quake.hotkey")).trimmed().isEmpty()) {
        return true;  // quake mode is off; the window is left exactly as it was
    }

    m_dropDown = true;
    // Qt::Tool is what gets WS_EX_TOOLWINDOW, which is the documented way a
    // window stays out of the taskbar and out of Alt+Tab. NOT
    // Qt::WindowDoesNotAcceptFocus, which maps to WS_EX_NOACTIVATE: that would
    // stop the drop-down ever taking the keyboard, which is the whole point.
    window->setFlags(window->flags() | Qt::Tool | Qt::FramelessWindowHint |
                     Qt::WindowStaysOnTopHint);

    QCoreApplication::instance()->installNativeEventFilter(this);
    // Live, so a combination that turned out to be taken can be changed and
    // retried without restarting the app that reported it.
    connect(registry, &settings::Registry::changed, this, [this](const QString& id) {
        if (id == QLatin1String("quake.hotkey")) {
            applyHotkey();
        }
    });

    applyHotkey();
    return m_registered;
}

void QuakeWindow::applyHotkey() {
    unregisterHotkey();
    m_error.clear();
    m_errorHint.clear();
    if (m_window.isNull() || m_settings == nullptr) {
        return;
    }
    const QString spelling = QString::fromStdString(m_settings->text("quake.hotkey")).trimmed();
    if (spelling.isEmpty()) {
        return;
    }
    const std::optional<Hotkey> hotkey = parseHotkey(spelling);
    if (!hotkey.has_value()) {
        m_error = tr("Krait does not understand the drop-down hotkey “%1”.").arg(spelling);
        m_errorHint = tr("Write it the way the other shortcuts are written, for example "
                         "Ctrl+Alt+` or Ctrl+Shift+F12. A combination with no Ctrl, Alt, Shift or "
                         "Win is only allowed for a function key.");
        reportHotkeyFailure();
        return;
    }

    // winId() creates the platform window if it does not exist yet, which is
    // what makes this safe before show(). Registering against the WINDOW rather
    // than the thread (a NULL hWnd) is deliberate: a thread-posted WM_HOTKEY
    // reaches Qt as "windows_dispatcher_MSG" instead, and tying the
    // registration to the window's lifetime is one less thing to unwind.
    auto* hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (RegisterHotKey(hwnd, kHotkeyId, hotkey->modifiers, hotkey->key) != FALSE) {
        m_hwnd = hwnd;
        m_registered = true;
        return;
    }

    // The common real failure, and the reason this returns something the user
    // can act on rather than doing nothing. Branch on the FALSE first: the
    // RegisterHotKey page names no error constant, so GetLastError only
    // sharpens the wording, it never decides whether this failed.
    const DWORD error = GetLastError();
    m_error = tr("Another program is already using %1, so Krait's drop-down hotkey is off.")
                  .arg(spelling);
    if (error != ERROR_HOTKEY_ALREADY_REGISTERED) {
        m_error = tr("Krait could not claim %1 as a system-wide hotkey, so the drop-down is off.")
                      .arg(spelling);
    }
    m_errorHint = tr("Pick a different combination in the quake.hotkey setting. Windows keeps some "
                     "for itself — F12 belongs to the debugger, and anything with the Windows key "
                     "usually belongs to Windows.");
    reportHotkeyFailure();
}

void QuakeWindow::reportHotkeyFailure() {
    // The window comes back FIRST. main() leaves it visible when the hotkey is
    // refused at startup, but quake.hotkey is hot-reloadable, so the same
    // failure can arrive while the drop-down is hidden — and then there would
    // be no hotkey and no window, which is an app the user cannot reach at all.
    // The banner below is also pointless on a window nobody can see.
    if (m_dropDown && !m_visible) {
        showDropDown();
    }
    emit hotkeyFailed(m_error, m_errorHint);
}

void QuakeWindow::unregisterHotkey() {
    if (!m_registered || m_hwnd == nullptr) {
        m_registered = false;
        return;
    }
    // The CACHED hWnd, and the same thread: UnregisterHotKey frees a key
    // "previously registered by the calling thread" and wants the window it was
    // registered against. Everything here runs on the GUI thread.
    UnregisterHotKey(static_cast<HWND>(m_hwnd), kHotkeyId);
    m_hwnd = nullptr;
    m_registered = false;
}

bool QuakeWindow::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* /*out*/) {
    // BOTH strings, on purpose. Qt sets "windows_generic_MSG" for messages sent
    // to toplevel windows and "windows_dispatcher_MSG" for system-wide ones
    // "such as messages from a registered hot key". We register against an
    // HWND, so ours arrives as the first — but a filter that tests only one of
    // them is a filter that silently stops working the day the registration
    // moves to the thread, and the second comparison costs nothing.
    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") {
        return false;
    }
    const auto* msg = static_cast<const MSG*>(message);
    if (msg->message != WM_HOTKEY || static_cast<int>(msg->wParam) != kHotkeyId) {
        return false;
    }
    toggle();
    return true;
}

void QuakeWindow::toggle() {
    if (m_window.isNull()) {
        return;
    }
    if (m_visible && m_window->isVisible()) {
        hideDropDown();
    } else {
        showDropDown();
    }
}

int QuakeWindow::heightPercent() const {
    return m_settings != nullptr ? static_cast<int>(m_settings->integer("quake.heightPercent"))
                                 : 45;
}

void QuakeWindow::showDropDown() {
    // What had the keyboard before we took it. Captured BEFORE anything is
    // shown, because from the next line on the answer is us.
    m_previousForeground = GetForegroundWindow();

    // The monitor the cursor is on, which is the one the user is looking at.
    // Falls back to the primary rather than giving up: screenAt() returns null
    // for a point on no screen, which a stale cursor position between a monitor
    // being unplugged and Qt noticing really does produce.
    const QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    const QRect target = quakeGeometry(screen->availableGeometry(), heightPercent());

    // setGeometry, never setScreen(). setScreen RECREATES the platform window
    // on the new screen — a new HWND, which would drop the hotkey registration
    // made against the old one — and Qt documents that it does not move the
    // window anyway when the screens are virtual siblings, which on Windows
    // they always are. The coordinates alone tell the platform which monitor.
    m_slide->stop();
    m_window->setGeometry(target);
    m_window->setVisible(true);
    m_visible = true;
    // raise() puts the window in front. It does NOT give it the keyboard —
    // those are different operations, and a drop-down that appears but cannot
    // be typed into is the failure this ordering exists to avoid.
    m_window->raise();
    m_window->requestActivate();
    // Windows can refuse the foreground to a process that does not hold the
    // input focus, and there is no return value that says so
    // (requestActivate() is void). Nothing here works around it: the documented
    // escapes all amount to lying to the foreground lock, and the honest
    // failure — the window is up and one click gives it the keyboard — is
    // better than a hack that stops working.

    m_slide->setStartValue(target.y() - target.height());
    m_slide->setEndValue(target.y());
    m_slide->start();
}

void QuakeWindow::hideDropDown() {
    m_slide->stop();
    m_visible = false;
    m_window->setVisible(false);
    // Give the keyboard back to whatever the user was doing. Hiding the window
    // alone leaves the focus nowhere in particular, which reads as the machine
    // having lost the plot.
    auto* previous = static_cast<HWND>(m_previousForeground);
    m_previousForeground = nullptr;
    if (previous != nullptr && IsWindow(previous) != FALSE) {
        SetForegroundWindow(previous);
    }
}

}  // namespace krait::app
