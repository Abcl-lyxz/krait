// WIN32_LEAN_AND_MEAN / NOMINMAX before anything reaches <windows.h>, for the
// same reason taskbar_progress.cpp and main.cpp do it: the min/max macros
// otherwise land in scope for the whole translation unit.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "notifier.h"

#include <windows.h>

#include <QCoreApplication>
#include <QWindow>
// After windows.h, which it depends on. shellapi.h is what the
// Shell_NotifyIconW documentation names as the include.
#include <shellapi.h>

#include <cstddef>

namespace krait::app {

namespace {

// Arbitrary but STABLE: (hWnd, uID) is the icon's identity for the NIM_MODIFY
// that shows each balloon and for the NIM_DELETE that stops it outliving the
// process. A per-notification id would add an icon per command.
constexpr UINT kIconId = 1;

// The identity fields every call needs.
//
// cbSize is the size of the WHOLE struct on Vista and later — learn.microsoft.com
// (NOTIFYICONDATAW) lists "6.0.6 or higher (Windows Vista and later) ->
// sizeof(NOTIFYICONDATA)" — and it is what selects the modern layout, so it is
// set here rather than per call site where one path could forget it.
NOTIFYICONDATAW identify(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kIconId;
    return nid;
}

// Truncating rather than refusing: szInfo is 256 WCHARs and szInfoTitle 64
// (both including the NUL), and a session title long enough to overflow one is
// still worth showing the front of. _TRUNCATE is what keeps the NUL, which a
// bare wcsncpy would drop at exactly the boundary that matters.
//
// Sizes come from ARRAYSIZE at the call sites, never from a literal: szTip is
// 64 or 128 depending on an #if in the SDK header.
void copyInto(wchar_t* dest, std::size_t size, const QString& text) {
    wcsncpy_s(dest, size, reinterpret_cast<const wchar_t*>(text.utf16()), _TRUNCATE);
}

}  // namespace

Notifier::~Notifier() {
    if (!m_added) {
        return;
    }
    // The classic failure this exists to prevent: a process that exits without
    // NIM_DELETE leaves a dead icon sitting in the notification area until
    // someone happens to hover over it and Explorer reaps it.
    //
    // The cached HWND, not a fresh one: main() destroys the QML engine — and
    // with it the window — before this runs, so there is nothing left to ask.
    // "When deleting a taskbar icon, specify only the cbSize, hWnd and uID
    // members", which is exactly what identify() fills.
    NOTIFYICONDATAW nid = identify(static_cast<HWND>(m_hwnd));
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_added = false;
}

bool Notifier::ensureIcon(QWindow* window) {
    if (m_added) {
        return true;
    }
    if (window == nullptr) {
        return false;
    }
    // handle() FIRST, and it is not a formality — the same trap
    // TaskbarProgress::apply() documents. winId() CREATES the platform window
    // when there is none, so calling it during teardown would mint a fresh
    // HWND and hang a shell icon on it. handle() only asks.
    if (window->handle() == nullptr) {
        return false;
    }
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd == nullptr) {
        return false;
    }

    NOTIFYICONDATAW nid = identify(hwnd);
    // NIF_SHOWTIP because NIM_SETVERSION below selects v4 behaviour, under
    // which the standard tooltip is suppressed unless it is asked for.
    // Deliberately NOT NIF_MESSAGE: that needs a uCallbackMessage and a window
    // procedure to receive it in, and this icon exists to carry balloons, not
    // to be clicked.
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    // Mandatory: "to display a notification, you must have an icon in the
    // notification area". SHARED — LoadIcon returns a cached handle, and
    // DestroyIcon on one is a documented bug ("do not use this function to
    // destroy a shared icon"), so nothing here ever frees it. The taskbar
    // sample on learn.microsoft.com does call DestroyIcon; that is only valid
    // for the non-shared icon it loads from a resource.
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    copyInto(nid.szTip, ARRAYSIZE(nid.szTip), QCoreApplication::applicationName());
    if (Shell_NotifyIconW(NIM_ADD, &nid) == FALSE) {
        return false;
    }
    // "NIM_SETVERSION must be called every time a notification area icon is
    // added (NIM_ADD)... The version setting is not persisted once a user logs
    // off." The version goes in the uVersion member of the union. A FALSE here
    // means the shell wants an older contract, which leaves a working v0 icon —
    // the balloon does not depend on v4, so it is not worth refusing over.
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    m_hwnd = hwnd;
    m_added = true;
    return true;
}

void Notifier::notify(QWindow* window, const QString& title, const QString& body) {
    if (body.isEmpty() || !ensureIcon(window)) {
        return;
    }

    NOTIFYICONDATAW nid = identify(static_cast<HWND>(m_hwnd));
    nid.uFlags = NIF_INFO;
    copyInto(nid.szInfo, ARRAYSIZE(nid.szInfo), body);
    copyInto(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle),
             title.isEmpty() ? QCoreApplication::applicationName() : title);
    // NIIF_INFO is a VALUE in the field NIIF_ICON_MASK covers, not a bit, so
    // only the genuine flags are OR'd onto it. NIIF_RESPECT_QUIET_TIME is the
    // documented way to stay out of the way during the shell's quiet period —
    // cheaper than calling SHQueryUserNotificationState before every command,
    // and it fails in the right direction: a balloon suppressed during quiet
    // time "is simply dismissed unshown" rather than queued up to arrive late.
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    if (Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE) {
        return;
    }

    // Explorer restarting takes every notification-area icon with it, and there
    // is no documented error code that separates that from any other failure —
    // so a failed MODIFY is read as "the icon is gone", re-added, and retried.
    //
    // ONCE. A second failure is a real one, and a notification nobody asked for
    // twice is not worth a retry loop on the UI thread. This is also why there
    // is no TaskbarCreated event filter here: the recovery is lazy, at the next
    // notification, which is the only moment the icon is needed at all.
    m_added = false;
    if (!ensureIcon(window)) {
        return;
    }
    nid.hWnd = static_cast<HWND>(m_hwnd);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

}  // namespace krait::app
