// WIN32_LEAN_AND_MEAN / NOMINMAX before anything reaches <windows.h>, for the
// same reason main.cpp does it: the min/max macros otherwise land in scope for
// the whole translation unit and break every std::min below.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "taskbar_progress.h"

#include <windows.h>

#include <QCoreApplication>
#include <QTimer>
#include <QWindow>
// After windows.h, which it depends on. shobjidl.h is what the ITaskbarList3
// documentation names as the include (the interface itself is declared in
// shobjidl_core.h, which shobjidl.h pulls in).
#include <shobjidl.h>

#include <algorithm>
#include <vector>

namespace krait::app {

namespace {

// Highest wins. Not the enum's numeric order, which is the wire order: OSC 9;4
// numbers Indeterminate 3 and Paused 4, and taking those at face value would
// let a "paused" tab outrank an errored one.
int severity(Progress state) {
    switch (state) {
    case Progress::Remove:
        return 0;
    case Progress::Set:
        return 1;
    case Progress::Indeterminate:
        return 2;
    case Progress::Paused:
        return 3;
    case Progress::Error:
        return 4;
    }
    return 0;
}

TBPFLAG toFlag(Progress state) {
    // Values verified against learn.microsoft.com (ITaskbarList3::SetProgressState):
    // TBPF_NOPROGRESS 0x0, TBPF_INDETERMINATE 0x1, TBPF_NORMAL 0x2,
    // TBPF_ERROR 0x4, TBPF_PAUSED 0x8, "all states are mutually exclusive".
    switch (state) {
    case Progress::Set:
        return TBPF_NORMAL;
    case Progress::Error:
        return TBPF_ERROR;
    case Progress::Indeterminate:
        return TBPF_INDETERMINATE;
    case Progress::Paused:
        // MS calls OSC 9;4 state 4 "Warning" and ConEmu calls it "paused"; both
        // land on the one yellow flag Windows has for it.
        return TBPF_PAUSED;
    case Progress::Remove:
        break;
    }
    return TBPF_NOPROGRESS;
}

}  // namespace

TaskbarState aggregateProgress(std::span<const TabProgress> tabs) {
    TaskbarState out;
    int best = 0;
    bool haveWinner = false;

    for (const TabProgress& tab : tabs) {
        const int rank = severity(tab.state);
        if (rank == 0) {
            continue;  // a tab with no progress votes for nothing
        }
        // Rule 3: an Error or Paused report with no percentage is a full bar.
        // Set with no percentage is 0 — a shell that says "I am 'somewhere' in
        // a determinate task" has told us nothing, and 0 is the honest bar.
        const int percent = tab.percent >= 0 ? tab.percent : (tab.state == Progress::Set ? 0 : 100);

        if (!haveWinner || rank > best) {
            best = rank;
            haveWinner = true;
            out.state = tab.state;
            out.percent = percent;
        } else if (rank == best) {
            out.percent = std::min(out.percent, percent);
        }
    }

    if (!haveWinner) {
        return {};
    }
    if (out.state == Progress::Remove || out.state == Progress::Indeterminate) {
        out.percent = 0;  // no bar to fill; pinned so equality is stable
    }
    return out;
}

TaskbarProgress::TaskbarProgress(QObject* parent) : QObject(parent) {
    m_since.start();
}

TaskbarProgress::~TaskbarProgress() {
    // QCoreApplication may already be gone if this outlived it; installing and
    // removing are both no-ops then, but the null check says so out loud.
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
    if (m_taskbar != nullptr) {
        m_taskbar->Release();
    }
}

void TaskbarProgress::attach(QWindow* window) {
    m_window = window;
    if (window == nullptr) {
        return;
    }
    // RegisterWindowMessage returns the same atom for the same string across
    // the whole session, so registering here is enough to recognise it later.
    m_buttonCreatedMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");
    QCoreApplication::instance()->installNativeEventFilter(this);
}

void TaskbarProgress::report(const QObject* tab, Progress state, int percent) {
    const auto at = std::ranges::find(m_tabs, tab, &std::pair<const QObject*, TabProgress>::first);
    if (at == m_tabs.end()) {
        m_tabs.emplace_back(tab, TabProgress{.state = state, .percent = percent});
    } else {
        at->second = TabProgress{.state = state, .percent = percent};
    }
    schedule();
}

void TaskbarProgress::forget(const QObject* tab) {
    const auto removed =
        std::ranges::remove(m_tabs, tab, &std::pair<const QObject*, TabProgress>::first);
    if (removed.empty()) {
        return;
    }
    m_tabs.erase(removed.begin(), removed.end());
    schedule();
}

TaskbarState TaskbarProgress::current() const {
    std::vector<TabProgress> values;
    values.reserve(m_tabs.size());
    for (const auto& entry : m_tabs) {
        values.push_back(entry.second);
    }
    return aggregateProgress(values);
}

bool TaskbarProgress::nativeEventFilter(const QByteArray& eventType, void* message,
                                        qintptr* /*result*/) {
    if (eventType != "windows_generic_MSG" || m_buttonCreatedMsg == 0) {
        return false;
    }
    const auto* msg = static_cast<const MSG*>(message);
    if (msg->message != m_buttonCreatedMsg) {
        return false;
    }
    // Explorer restarting re-sends this and invalidates the old interface, so
    // the pointer is dropped rather than reused.
    if (m_taskbar != nullptr) {
        m_taskbar->Release();
        m_taskbar = nullptr;
    }
    m_buttonReady = true;
    // Whatever the tabs were saying is now showable. Never consume the message:
    // other filters (and Qt itself) are entitled to see it.
    m_applied = TaskbarState{};
    schedule();
    return false;
}

void TaskbarProgress::schedule() {
    if (m_pending) {
        return;  // a poke is already queued; it will pick up the newest value
    }
    const qint64 waited = m_since.elapsed();
    if (waited >= kMinIntervalMs) {
        apply();
        return;
    }
    m_pending = true;
    QTimer::singleShot(static_cast<int>(kMinIntervalMs - waited), this, [this] {
        m_pending = false;
        apply();
    });
}

void TaskbarProgress::apply() {
    m_since.restart();

    const TaskbarState wanted = current();
    if (wanted == m_applied) {
        return;
    }
    if (m_window == nullptr || !m_buttonReady) {
        // Nothing to draw on yet. Deliberately does NOT record m_applied, so
        // the state is re-applied once the button exists.
        return;
    }

    if (m_taskbar == nullptr) {
        // Qt has already put this thread in an STA (it calls OleInitialize on
        // the GUI thread — doc.qt.io/qt-6/windows-issues.html), so calling
        // CoInitializeEx here would at best be redundant and at worst change
        // the apartment model out from under Qt's own OLE use.
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&m_taskbar)))) {
            m_taskbar = nullptr;
            return;
        }
        // "This method must be called before any other ITaskbarList methods can
        // be called... If the method fails, no other methods can be called."
        if (FAILED(m_taskbar->HrInit())) {
            m_taskbar->Release();
            m_taskbar = nullptr;
            return;
        }
    }

    // handle() FIRST, and it is not a formality. winId() *creates* the platform
    // window when there is none, and apply() is reachable synchronously from
    // ~TerminalItem via forget() — which runs inside ~QQuickWindow, after the
    // window has already destroyed its native half. Calling winId() there would
    // mint a fresh HWND during teardown and then make a cross-process COM call
    // on it. handle() only asks; it never builds.
    if (m_window->handle() == nullptr) {
        return;
    }
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (hwnd == nullptr) {
        return;  // creation failed; there is no button to poke
    }

    // Order matters: SetProgressValue "assumes the TBPF_NORMAL state even if it
    // is not explicitly set" and "overrides and clears the TBPF_INDETERMINATE
    // state", so the value goes first and the state has the last word.
    if (wanted.state == Progress::Set || wanted.state == Progress::Error ||
        wanted.state == Progress::Paused) {
        m_taskbar->SetProgressValue(hwnd, static_cast<ULONGLONG>(wanted.percent), 100);
    }
    m_taskbar->SetProgressState(hwnd, toFlag(wanted.state));
    m_applied = wanted;
}

}  // namespace krait::app
