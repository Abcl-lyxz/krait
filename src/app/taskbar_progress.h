#pragma once

#include "core/parser/osc.h"

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>

#include <span>
#include <utility>
#include <vector>

class QWindow;
struct ITaskbarList3;

namespace krait::app {

// OSC 9;4 taskbar progress (T67).
//
// The sequence is per TAB — every shell in the window can report its own — but
// Windows gives the window ONE taskbar button, so N reports have to collapse
// into one bar. That collapse is `aggregateProgress` below, kept as a pure
// function so the rule is testable without a taskbar, a COM apartment or a
// window: the OS half of this file is thin on purpose and the decision half is
// where the behaviour lives.

using Progress = core::vt::OscAction::Progress;

// One tab's last report.
struct TabProgress {
    Progress state = Progress::Remove;
    // 0-100, or -1 when the tab did not supply one (states 2 and 4 may omit it).
    int percent = -1;
};

// What the single taskbar button should show.
struct TaskbarState {
    Progress state = Progress::Remove;
    // 0-100. Meaningless when `state` is Remove or Indeterminate, and left at 0
    // there so two equal states compare equal and the throttle can skip them.
    int percent = 0;

    bool operator==(const TaskbarState& other) const = default;
};

// The aggregation rule, stated once:
//
//   1. SEVERITY WINS. Error > Paused > Indeterminate > Set > Remove. A tab
//      whose build failed must not be silently outvoted by another tab that
//      happens to be 60% through a download — the whole value of the taskbar
//      bar is that it says "something needs you" from behind another window,
//      and an error is the strongest form of that. It is also the only ordering
//      that cannot lose information: a percentage is still visible on the tab
//      itself, an error state on a hidden tab is not.
//   2. WITHIN the winning state, the LOWEST percentage. The button reaches
//      100% only when every tab reporting does, which is the reading a user
//      actually acts on ("is it done yet?"). Taking the maximum would show a
//      finished bar while work was still running.
//   3. An Error or Paused tab that supplied no percentage counts as 100 — a
//      full red bar is what a failure looks like, and an empty one is
//      indistinguishable from no progress at all.
TaskbarState aggregateProgress(std::span<const TabProgress> tabs);

// Drives the real Windows taskbar button.
//
// Owned by main() rather than a singleton: it installs a native event filter on
// the application and holds a COM interface, and both want to be released while
// QGuiApplication is still alive — which a function-local static cannot promise.
class TaskbarProgress : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

  public:
    explicit TaskbarProgress(QObject* parent = nullptr);
    ~TaskbarProgress() override;

    // The window whose button this drives. Borrowed; the QML engine owns it.
    void attach(QWindow* window);

    // `tab` identifies the reporter and is never dereferenced — only compared.
    void report(const QObject* tab, Progress state, int percent);
    void forget(const QObject* tab);

    // Waits for the TaskbarButtonCreated message. Microsoft is explicit that it
    // "must be received by your application before it calls any ITaskbarList3
    // method", and Qt Quick has no wndproc of its own to receive it in.
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

    // For the tests: what the aggregate currently says, without a taskbar.
    TaskbarState current() const;

  private:
    void schedule();
    void apply();

    // Remote input can repaint the bar as fast as it can emit thirteen bytes,
    // and every poke is a cross-process COM call. 100 ms is well under what an
    // eye resolves on a taskbar and bounds a hostile stream to ten calls a
    // second (rules/net.md).
    static constexpr int kMinIntervalMs = 100;

    // QPointer rather than a raw borrow, for the window outliving nobody: the
    // QML engine destroys the window before main()'s locals go.
    //
    // It is NOT sufficient on its own, and the reason is worth stating because
    // it is easy to believe otherwise. QPointer clears at the top of ~QObject,
    // but ~QQuickWindow deletes its content item — and so every tab under it —
    // in its own body, BEFORE that. So a tab's destructor calling forget()
    // still sees a non-null pointer here. The check that actually holds is
    // `handle() == nullptr` in apply(); see the note there.
    QPointer<QWindow> m_window;
    ITaskbarList3* m_taskbar = nullptr;  // COM, AddRef'd; released in the dtor
    unsigned int m_buttonCreatedMsg = 0;
    bool m_buttonReady = false;
    // Small and searched linearly: this is one entry per open tab, so a map
    // would cost more in ceremony than the scan ever costs in time.
    std::vector<std::pair<const QObject*, TabProgress>> m_tabs;
    TaskbarState m_applied;
    QElapsedTimer m_since;
    bool m_pending = false;
};

}  // namespace krait::app
