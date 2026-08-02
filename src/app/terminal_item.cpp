#include "terminal_item.h"

#include "backend_factory.h"
#include "capture.h"
#include "core/grid/search.h"
#include "core/unicode/width.h"
#include "error_banner.h"
#include "input/ime.h"
#include "input/keymap.h"
#include "input/mouse.h"
#include "input/paste.h"
#include "net/remote_text.h"
#include "notifier.h"
#include "render/shaper/run_splitter.h"
#include "settings/paths.h"
#include "settings/registry.h"
#include "theme/store.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>

namespace krait::app {
namespace {

// Tried in order. The first that DirectWrite reports installed wins, so nothing
// is hardcoded to a font that may be absent (T31 makes this a setting).
constexpr std::array<std::string_view, 5> kFontCandidates{
    "Cascadia Mono", "Cascadia Code", "Consolas", "Lucida Console", "Courier New"};

// T70. The three logging settings, named once so a typo is a link error rather
// than a silently-defaulted read.
constexpr std::string_view kLogTemplate = "logging.pathTemplate";
constexpr std::string_view kLogFormat = "logging.format";
constexpr std::string_view kLogIncludeInput = "logging.includeInput";

// The schema constrains this to the three choices, so anything else is a file
// written by a newer build — and Escaped, the default, is the safe read.
LogFormat logFormatFromName(const QString& name) {
    if (name == QStringLiteral("raw")) {
        return LogFormat::Raw;
    }
    if (name == QStringLiteral("text")) {
        return LogFormat::Text;
    }
    return LogFormat::Escaped;
}

QShader loadShader(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

// What the command line asked for, waiting for the first terminal to claim it.
// File-scope because it models something genuinely process-global — argv — and
// because QML constructs the item, so there is no other moment to hand it over
// before geometry arrives and the first shell starts.
std::optional<session::Profile> g_launchProfile;

// What every terminal borrows. See TerminalItem::setServices for why these are
// file-scope rather than passed in: QML builds terminals on demand now.
settings::Registry* g_registry = nullptr;
net::Vault* g_vault = nullptr;
session::ProfileStore* g_store = nullptr;
TaskbarProgress* g_taskbar = nullptr;
Notifier* g_notifier = nullptr;
theme::ThemeStore* g_themes = nullptr;

// The theme, translated for the renderer. render::Theme is a flat block of
// 0xRRGGBB and app::theme::Theme carries the same numbers plus a name and the
// chrome, so this is a copy rather than a conversion — which is the whole
// reason both sides agreed on that representation.
render::Theme renderTheme() {
    render::Theme out;
    if (g_themes == nullptr) {
        return out;
    }
    const theme::Theme& live = g_themes->current();
    out.ansi = live.ansi;
    out.fg = live.fg;
    out.bg = live.bg;
    out.cursor = live.cursor;
    out.cursorText = live.cursorText;
    out.selectionBg = live.selectionBg;
    out.highlightBg = live.highlightBg;
    return out;
}

}  // namespace

void TerminalItem::setLaunchProfile(const session::Profile& profile) {
    g_launchProfile = profile;
}

void TerminalItem::setServices(settings::Registry* registry, net::Vault* vault,
                               session::ProfileStore* store) {
    g_registry = registry;
    g_vault = vault;
    g_store = store;
}

void TerminalItem::setTaskbar(TaskbarProgress* taskbar) {
    g_taskbar = taskbar;
}

void TerminalItem::setNotifier(Notifier* notifier) {
    g_notifier = notifier;
}

void TerminalItem::setThemes(theme::ThemeStore* themes) {
    g_themes = themes;
}

void TerminalItem::applyTheme() {
    if (!m_builder) {
        return;  // no renderer yet; ensureRenderer() reads the live theme itself
    }
    m_builder->setTheme(renderTheme());
    // rules/render.md names a theme change as one of the three cases where a
    // full-frame redraw is correct — the other two being resize and a font
    // change. Every cached row holds resolved colours, so a partial repaint
    // would leave the untouched rows in the previous palette.
    m_builder->invalidate();
    rebuildFrame();
    update();
    // T83. AFTER the repaint, so an application that redraws on the
    // notification draws over a window already wearing the new colours.
    reportColorScheme();
}

TerminalItem::TerminalItem() {
    m_vault = g_vault;
    m_store = g_store;
    // T65. Built here rather than lazily so the `files` property can be
    // CONSTANT: QML binds to it before any session exists, and a property that
    // turns from null into an object needs a NOTIFY that nothing else wants.
    m_files = new SftpModel(this);  // owned by this
    // Through setSettings(), not a bare assignment: it also subscribes to hot
    // reloads and applies the current values, and a tab opened after startup
    // needs both exactly as much as the first one did.
    setSettings(g_registry);
    // T75. Every tab repaints on a theme change, including tabs opened long
    // after startup — which is why this is per-instance rather than a sweep
    // main() does once over whatever existed then.
    if (g_themes != nullptr) {
        connect(g_themes, &theme::ThemeStore::changed, this, &TerminalItem::applyTheme);
    }
    // Claimed, not copied: the SECOND terminal (a new tab) must open a default
    // shell rather than a second copy of whatever was on the command line.
    if (g_launchProfile) {
        m_profile = *g_launchProfile;
        g_launchProfile.reset();
    }
    setAcceptedMouseButtons(Qt::AllButtons);
    // Without this Qt never routes composition to us and Thai or Japanese input
    // silently falls back to whatever the IME does with an unaware widget.
    setFlag(ItemAcceptsInputMethod, true);
    setFlag(ItemIsFocusScope, false);
    setFocus(true);

    bool ok = false;
    const int frames = qEnvironmentVariableIntValue("KRAIT_TERM_BENCH", &ok);
    if (ok && frames > 0) {
        m_benchFrames = frames;
        // Watchdog: a failed pipeline would otherwise idle forever and stall an
        // unattended bench run.
        QTimer::singleShot(60000, this, [] {
            qWarning("bench: timed out");
            QCoreApplication::exit(2);
        });
        if (qEnvironmentVariableIsSet("KRAIT_BENCH_4K")) {
            // Match the M0 baseline exactly, or "flood >= M0" compares nothing:
            // that run used a fixed 4K colour buffer and a 240x63 grid, both
            // independent of the window size.
            setFixedColorBufferWidth(3840);
            setFixedColorBufferHeight(2160);
            m_benchCols = 240;
            m_benchRows = 63;
        }
    }
}

void TerminalItem::adoptBackend(net::IBackend* backend) {
    m_backend = backend;
    m_exited = false;
    m_reconnecting = false;
    // EVERY lambda below re-checks that `backend` is still the current one, and
    // that is not belt-and-braces — it is the whole correctness argument for
    // switching sessions.
    //
    // disconnect() does NOT cancel emissions that are already in flight.
    // ConptyBackend posts its own invokeMethod to ITSELF, so disconnecting it
    // really does drop those. SshBackend emits straight from the worker thread,
    // and a queued cross-thread emission posts a QMetaCallEvent to the
    // RECEIVER — this item — where disconnect cannot reach it. Without the
    // guard, the last chunk from the host you just navigated away from is
    // parsed into the grid of the host you navigated to; a DA/DSR inside it
    // answers back over the NEW connection, and a credential prompt from the
    // old host collects a password and hands it to the new one.
    //
    // Qt::QueuedConnection explicitly, per rules/cpp.md: these cross a thread
    // boundary and must not be left to AutoConnection to work out.
    connect(
        m_backend, &net::IBackend::outputReceived, this,
        [this, backend](const QByteArray& bytes) {
            if (backend != m_backend) {
                return;
            }
            handleOutput(bytes);
        },
        Qt::QueuedConnection);
    connect(
        m_backend, &net::IBackend::errorOccurred, this,
        [this, backend](const QString& code, const QString& message) {
            if (backend != m_backend) {
                return;
            }
            qWarning("backend error [%s]: %s", qPrintable(code), qPrintable(message));
            const ErrorBanner banner = describeError(code);
            // The backend's own message is the DETAIL, and the taxonomy
            // supplies the headline: a raw Win32 string is not a sentence a
            // user can act on, but it is exactly what a bug report needs.
            const QString hint =
                banner.hint.isEmpty() ? message : banner.hint + QStringLiteral(" ") + message;
            emit errorRaised(banner.message, hint);
        },
        Qt::QueuedConnection);
    connect(
        m_backend, &net::IBackend::exited, this,
        [this, backend](int exitCode) {
            if (backend != m_backend) {
                return;
            }
            qInfo("shell exited (%d)", exitCode);
            m_exited = true;
            // A shell that ran `exit` is NOT an error. Raising a banner here is
            // how a banner teaches people to ignore banners.
            if (exitCode != 0) {
                emit errorRaised(tr("The shell exited with code %1.").arg(exitCode), QString());
            }
        },
        Qt::QueuedConnection);

    // The SSH-only half. Cast rather than a virtual on IBackend: host-key trust
    // and credential prompts are not things a serial port or a local shell has,
    // and putting them on the seam would give four backends a method that can
    // only ever return "not me".
    auto* ssh = qobject_cast<net::SshBackend*>(m_backend);
    // Before the early return: the panel has to hear about a NON-SSH session
    // too, or a tab re-pointed at a local shell keeps offering transfers over
    // the connection it no longer has.
    m_files->attach(ssh, QString::fromStdString(m_profile.host));
    if (ssh == nullptr) {
        return;
    }
    connect(
        ssh, &net::SshBackend::hostKeyPrompt, this,
        [this, backend](int state, const QString& detail) {
            if (backend != m_backend) {
                return;
            }
            const HostKeyPrompt prompt = describeHostKey(static_cast<net::HostKeyState>(state));
            emit hostKeyPromptRequested(prompt.message, detail, prompt.askable);
        },
        Qt::QueuedConnection);
    connect(
        ssh, &net::SshBackend::credentialPrompt, this,
        [this, backend](const QString& prompt, bool echo) {
            // The guard matters most HERE: a prompt from the connection the
            // user just left would otherwise collect a password and
            // respondCredential() would hand it to whatever is current now.
            if (backend != m_backend) {
                return;
            }
            emit credentialPromptRequested(prompt, echo);
        },
        Qt::QueuedConnection);
    connect(
        ssh, &net::SshBackend::connected, this,
        [this, backend] {
            if (backend != m_backend) {
                return;
            }
            // Clears whatever the last reconnect notice said. A banner that
            // stays up after the thing it describes is over is a banner people
            // stop reading.
            m_reconnecting = false;
            emit connectionNotice(QString());
        },
        Qt::QueuedConnection);
    connect(
        ssh, &net::SshBackend::forwardsChanged, this,
        [this, backend](const QVariantList& rows) {
            if (backend != m_backend) {
                return;
            }
            m_tunnels = rows;
            emit tunnelsChanged();
        },
        Qt::QueuedConnection);
    connect(
        ssh, &net::SshBackend::reconnecting, this,
        [this, backend](int attempt, int ofAttempts, int delayMs) {
            if (backend != m_backend) {
                return;
            }
            m_reconnecting = true;
            emit connectionNotice(tr("Connection lost. Reconnecting in %1 s (%2 of %3)…")
                                      .arg(QString::number((delayMs + 999) / 1000),
                                           QString::number(attempt), QString::number(ofAttempts)));
        },
        Qt::QueuedConnection);
}

void TerminalItem::setVault(net::Vault* vault) {
    m_vault = vault;
}

void TerminalItem::sendInput(const QByteArray& bytes) {
    // The item outlives its backend and predates it: a bench run has none by
    // design, and openProfile() leaves a window with none while the next one is
    // built. Every keypress, paste, mouse report, IME commit and answerback
    // funnels through here so that window is one check rather than five.
    if (m_backend == nullptr || bytes.isEmpty()) {
        return;
    }
    // Opt-in, and off by default (rules/net.md: never in logs). This is the
    // stream a password typed at an echo-off prompt travels in, and it is the
    // only one where turning logging on can capture a secret the far end never
    // echoed back. See SessionLog for what output-only does and does not buy.
    if (m_logInput) {
        m_log.writeInput(bytes);
    }
    m_backend->writeInput(bytes);
}

void TerminalItem::resetSession() {
    if (m_backend != nullptr) {
        net::IBackend* old = m_backend;
        // Cleared FIRST. Every connection made in adoptBackend() compares its
        // captured pointer against this member, so from here on any emission
        // still in flight from `old` is dropped rather than delivered into the
        // session that replaces it.
        m_backend = nullptr;
        old->disconnect(this);

        // Unparented and torn down OFF the UI thread. stop() joins a worker
        // that can be sitting inside an uninterruptible libssh call
        // (ssh_connect against a host that is not answering blocks until the
        // connect timeout), and rules/net.md is explicit that a network wait
        // never runs on the UI thread — switching sessions must not freeze the
        // window for fifteen seconds.
        //
        // Unparenting matters: while the pool thread is inside stop(), this
        // item must not be able to delete the backend out from under it as a
        // child. deleteLater() then posts the deletion back to the GUI thread,
        // where the object lives.
        old->setParent(nullptr);
        QThreadPool::globalInstance()->start([old] {
            old->stop();
            old->deleteLater();
        });
    }
    m_session.reset();
    m_started = false;
    // The panel goes with the connection: a listing left on screen after the
    // session it came from is a listing of a folder nobody can reach.
    m_files->attach(nullptr);
    if (!m_tunnels.isEmpty()) {
        // The tunnels belonged to the connection that just went. Leaving them
        // on screen would show a pane full of listeners that no longer exist.
        m_tunnels.clear();
        emit tunnelsChanged();
    }
}

void TerminalItem::openProfile(const session::Profile& profile) {
    m_profile = profile;
    // Before resetSession(), so the first bytes of the new connection are
    // already being matched: a login banner is exactly the kind of thing people
    // write a trigger for.
    applyTriggers();
    resetSession();
    // Nothing else to do before the first geometry: ensureStarted() bails until
    // the grid size is known, and updateGrid() calls it again once it is.
    ensureStarted();
    if (m_started) {
        rebuildFrame();
        update();
    }
    emit sessionChanged();
}

bool TerminalItem::openProfileById(const QString& profileId) {
    if (m_store == nullptr) {
        return false;
    }
    const session::Profile* raw = m_store->find(profileId.toStdString());
    if (raw == nullptr) {
        return false;
    }
    // resolve(), not the stored profile: the store holds only the keys each
    // profile owns, so one inheriting its user from [folders."prod"] would
    // otherwise reach the backend with an empty user.
    openProfile(m_store->resolve(*raw));
    return true;
}

QString TerminalItem::sessionTitle() const {
    if (!m_profile.name.empty()) {
        return QString::fromStdString(m_profile.name);
    }
    // An unnamed profile is the default local shell. "Shell" rather than the
    // executable name: the tab says what it is, not how it was spelled.
    return tr("Shell");
}

QString TerminalItem::sessionAccent() const {
    return QString::fromStdString(m_profile.accent);
}

void TerminalItem::respondHostKey(bool trust) {
    if (auto* ssh = qobject_cast<net::SshBackend*>(m_backend); ssh != nullptr) {
        ssh->respondHostKey(trust);
    }
}

void TerminalItem::setHexdump(bool on) {
    if (m_hexdump == on) {
        return;
    }
    m_hexdump = on;
    // The offset restarts with the view. It counts bytes SEEN in this dump,
    // and carrying a count across a period when nothing was being counted
    // would make it a number that means nothing.
    m_hexdumpOffset = 0;
    if (m_session) {
        // A banner line so the transition is legible in the scrollback rather
        // than the output silently changing shape mid-screen.
        const QByteArray note = on ? QByteArrayLiteral("\r\n--- hexdump on ---\r\n")
                                   : QByteArrayLiteral("\r\n--- hexdump off ---\r\n");
        m_session->feed({reinterpret_cast<const std::uint8_t*>(note.constData()),
                         static_cast<std::size_t>(note.size())});
        rebuildFrame();
        update();
    }
}

QString TerminalItem::toggleLogging() {
    if (m_log.isOpen()) {
        m_log.close();
        m_logPath.clear();
        emit loggingChanged();
        return {};
    }
    namespace ks = settings;
    const ks::Resolution dir = ks::resolveConfigDir(
        ks::systemPathInputs(), [](const QString& path) { return QFile::exists(path); });
    const QString tmpl =
        m_settings == nullptr ? QString() : QString::fromStdString(m_settings->text(kLogTemplate));
    const QString format =
        m_settings == nullptr ? QString() : QString::fromStdString(m_settings->text(kLogFormat));
    const LogFields fields{.session = sessionTitle(),
                           .host = QString::fromStdString(m_profile.host)};
    const QString path = expandLogPath(dir.dir, tmpl, fields, QDateTime::currentDateTime());
    if (!m_log.open(path, logFormatFromName(format))) {
        emit errorRaised(tr("Could not start logging."), m_log.error());
        return {};
    }
    m_logPath = path;
    emit loggingChanged();
    return path;
}

void TerminalItem::setCopyMode(bool on) {
    if (m_copyMode == on || !m_session) {
        return;
    }
    m_copyMode = on;
    if (on) {
        // OUTPUT KEEPS ARRIVING, and the view does not freeze. Grid already
        // solves the case that matters: pushToScrollback() bumps m_viewOffset
        // whenever the viewport is scrolled up, so once the user is reading
        // history the rows under them do not move however much the far end
        // prints. Sitting at the live bottom, the screen scrolls exactly as it
        // does outside copy mode — which is what staying at the bottom asks for
        // — and the first `k` moves into history and pins it.
        //
        // The alternative, freezing the viewport on entry, was rejected: it
        // needs the cursor tracked in absolute-line space while the yank needs
        // it in the viewport space render::Selection uses, and two coordinate
        // systems for one cursor is how a selection ends up describing text the
        // user is not looking at.
        //
        // Starts where the shell's cursor is when the view is live, and at the
        // bottom of what is on screen when it is not — in both cases on a row
        // the user is already looking at.
        const core::vt::Grid& grid = m_session->grid();
        m_copyCursor = {};
        m_copyCursor.row = grid.viewOffset() == 0 ? grid.row : std::max(0, m_rows - 1);
        m_copyCursor.col = grid.viewOffset() == 0 ? grid.col : 0;
        m_copyCursor.anchorRow = m_copyCursor.row;
        m_copyCursor.anchorCol = m_copyCursor.col;
    } else {
        // Leaving snaps back to the live screen, the same thing a keypress does
        // outside copy mode. A mode you left that silently left you reading
        // history is a mode people think is still on.
        m_session->grid().scrollViewToBottom();
        m_selection.active = false;
    }
    emit copyModeChanged();
    rebuildFrame();
    update();
}

// The selection copy mode is currently describing, in the viewport coordinates
// render::Selection uses. Line mode widens to whole rows at read time rather
// than moving the anchor, so a `V` then `v` gives back the character selection
// the user had rather than a row-wide one.
render::Selection TerminalItem::copySelectionRange() const {
    render::Selection sel{.active = m_copyCursor.select != input::Selecting::Off,
                          .anchorRow = m_copyCursor.anchorRow,
                          .anchorCol = m_copyCursor.anchorCol,
                          .cursorRow = m_copyCursor.row,
                          .cursorCol = m_copyCursor.col};
    if (m_copyCursor.select == input::Selecting::Line) {
        const bool forward = sel.cursorRow >= sel.anchorRow;
        sel.anchorCol = forward ? 0 : std::max(0, m_cols - 1);
        sel.cursorCol = forward ? std::max(0, m_cols - 1) : 0;
    }
    return sel;
}

bool TerminalItem::handleCopyKey(QKeyEvent* event) {
    const input::Command command = input::translateCopyKey(event->key(), event->modifiers());
    if (command.kind == input::Command::Kind::None) {
        return false;  // not ours: the chrome still gets its shortcuts
    }
    core::vt::Grid& grid = m_session->grid();
    switch (command.kind) {
    case input::Command::Kind::Leave:
        setCopyMode(false);
        return true;
    case input::Command::Kind::Yank:
        // Through copySelection(), not a second extractor: wrap joining and
        // trailing-space trimming have to match what a mouse drag produces.
        m_selection = copySelectionRange();
        copySelection();
        setCopyMode(false);
        return true;
    case input::Command::Kind::Select:
        // Pressing the same one again turns it off, which is what vim does and
        // what anyone who pressed `v` by accident will try.
        m_copyCursor.select =
            m_copyCursor.select == command.select ? input::Selecting::Off : command.select;
        if (m_copyCursor.select == input::Selecting::Off) {
            m_copyCursor.anchorRow = m_copyCursor.row;
            m_copyCursor.anchorCol = m_copyCursor.col;
        }
        break;
    case input::Command::Kind::Move: {
        const int scroll =
            input::applyMotion(m_copyCursor, command.motion, m_viewport, grid.clusters());
        if (scroll != 0) {
            grid.scrollView(scroll);
        }
        break;
    }
    case input::Command::Kind::None:
        return false;
    }
    m_selection = copySelectionRange();
    rebuildFrame();
    update();
    return true;
}

void TerminalItem::raiseError(const QString& message, const QString& hint) {
    emit errorRaised(message, hint);
}

void TerminalItem::applyTriggers() {
    const bool enabled = m_settings == nullptr || m_settings->boolean("triggers.enabled");
    // `triggers.allowSend` is read HERE and nowhere else: the engine drops every
    // send action at compile time when it is off, so there is exactly one place
    // in the build where the answer can become yes.
    const bool allowSend =
        enabled && m_settings != nullptr && m_settings->boolean("triggers.allowSend");
    m_triggers.setTriggers(enabled ? session::parseTriggers(m_profile.triggers)
                                   : std::vector<session::Trigger>{},
                           allowSend);
    if (!m_triggerClock.isValid()) {
        m_triggerClock.start();
    }
    // The log destination may have just changed, and the stripper's state
    // belongs to a rule set that no longer exists. Both are reopened lazily.
    m_triggerLog.close();
    m_triggerLogFailed = false;
    m_stripState = session::StripState::Ground;
    for (const std::string& error : m_triggers.errors()) {
        // A rule that will never match is worth one banner when it is loaded,
        // not silence until the user wonders why nothing highlights.
        emit errorRaised(tr("A trigger pattern is not valid and was skipped."),
                         QString::fromStdString(error));
    }

    m_snippetList = session::parseSnippets(m_profile.snippets);
    m_snippets.clear();
    for (const session::Snippet& snippet : m_snippetList) {
        QVariantMap row;
        row["name"] = QString::fromStdString(snippet.name);
        // What will actually be sent, on one line, so a snippet whose name says
        // "restart" cannot quietly be something else. Escapes stay escaped for
        // the same reason: an invisible CR in a preview is a preview that lies.
        row["preview"] = QString::fromStdString(session::escapeText(snippet.text));
        m_snippets.append(row);
    }
}

void TerminalItem::logTrigger(const QString& text) {
    if (m_triggerLogFailed) {
        return;  // said once, at the rate the user's own config caused it
    }
    if (!m_triggerLog.is_open()) {
        QString path = m_settings != nullptr
                           ? QString::fromStdString(m_settings->text("triggers.logFile"))
                           : QString();
        if (path.isEmpty()) {
            namespace ks = settings;
            const ks::Resolution dir = ks::resolveConfigDir(
                ks::systemPathInputs(), [](const QString& at) { return QFile::exists(at); });
            path = dir.dir + "/logs/triggers.log";
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        // The wide overload, not toStdString(): the narrow one decodes with the
        // active code page, so a config directory under a Thai user name never
        // opens. MSVC-specific, like the rest of this file's Win32 assumptions.
        m_triggerLog.open(path.toStdWString(), std::ios::app | std::ios::binary);
        if (!m_triggerLog.is_open()) {
            m_triggerLogFailed = true;
            emit errorRaised(tr("Could not write the trigger log."), path);
            return;
        }
    }
    // Appended, never rewritten, and the session name is in every line: one
    // file serves every tab, and a line that does not say which one is a line
    // nobody can act on.
    const QString line = QDateTime::currentDateTime().toString(Qt::ISODate) + "\t" +
                         sessionTitle() + "\t" + text + "\n";
    const QByteArray bytes = line.toUtf8();
    m_triggerLog.write(bytes.constData(), bytes.size());
    m_triggerLog.flush();
}

void TerminalItem::runTriggers(const QByteArray& bytes) {
    if (m_triggers.empty()) {
        return;  // the common case, and it has to cost one branch
    }
    // The control layer comes out first. Matching the raw bytes would let a
    // remote bait a pattern from inside an escape payload it knows will never
    // be drawn, and would anchor `^` to wherever the last write happened to
    // stop rather than to a line.
    const std::string text = session::plainText(
        {bytes.constData(), static_cast<std::size_t>(bytes.size())}, m_stripState);
    const auto nowMs = static_cast<std::uint64_t>(m_triggerClock.elapsed());

    for (const session::TriggerHit& hit : m_triggers.feed(text, nowMs)) {
        // The matched text is REMOTE-chosen and about to sit next to words the
        // user trusts. rules/net.md, and the helper that already solves exactly
        // this: controls out, length and line count capped.
        const QString matched = net::sanitizeRemoteText(hit.matched, 120, 1);
        if (hit.log) {
            logTrigger(matched);
        }
        if (hit.notify) {
            if (QWindow* const own = window();
                own != nullptr && g_notifier != nullptr && QGuiApplication::focusWindow() != own) {
                g_notifier->notify(own, sessionTitle(), matched);
            }
            // A per-tab banner as well as (or instead of) the balloon: the
            // banner is the surface that survives being missed, and rules/ui.md
            // makes it the only in-session error surface. Never modal.
            emit triggerMatched(tr("A trigger matched: %1").arg(matched));
        }
        if (!hit.send.empty()) {
            // Through preparePaste like every other block of text entering a
            // pty: the send text is user-authored, but it is also the ONE
            // action a remote host can fire, so it gets the same ESC/C0 strip
            // and bracketed-paste wrapping a clipboard paste does. The risk
            // classification is deliberately ignored — the user wrote this into
            // their own profile, and a confirmation banner on an automated
            // response is a response that never happens.
            const auto guarded = input::preparePaste(QString::fromStdString(hit.send),
                                                     m_session->grid().bracketedPaste);
            sendPaste(guarded.bytes);
        }
    }
}

void TerminalItem::sendSnippet(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_snippetList.size() || !m_session) {
        return;
    }
    const auto guarded = input::preparePaste(
        QString::fromStdString(m_snippetList[static_cast<std::size_t>(index)].text),
        m_session->grid().bracketedPaste);
    sendPaste(guarded.bytes);
}

bool TerminalItem::sendBroadcast(const QString& text) {
    // Reported honestly rather than swallowed. A broadcast that counts a dead
    // tab as delivered is the failure mode the whole interlock exists to
    // prevent: the user believes twelve hosts ran the command and nine did.
    if (!m_session || !m_started || m_backend == nullptr || m_exited || m_reconnecting) {
        return false;
    }
    // The same path a paste and a snippet take — this must not become the one
    // way text reaches a pty without the ESC/C0 strip (rules/net.md).
    const auto guarded = input::preparePaste(text, m_session->grid().bracketedPaste);
    sendPaste(guarded.bytes);
    // Enter, OUTSIDE the brackets. A CR inside a bracketed paste is inserted
    // as a literal newline by readline rather than accepted as a command, so
    // wrapping it would leave the line sitting unrun on every host — which is
    // the same lie as swallowing it, arriving one step later.
    sendInput(QByteArrayLiteral("\r"));
    return true;
}

void TerminalItem::respondCredential(const QString& text, bool remember) {
    if (auto* ssh = qobject_cast<net::SshBackend*>(m_backend); ssh != nullptr) {
        ssh->respondCredential(text, remember);
    }
}

TerminalItem::~TerminalItem() {
    if (m_backend != nullptr) {
        m_backend->stop();
    }
    // A closed tab stops voting. Without this the taskbar keeps showing the
    // progress of a shell that no longer exists, and nothing will ever clear it
    // because the only thing that could has been deleted.
    if (g_taskbar != nullptr) {
        g_taskbar->forget(this);
    }
}

bool TerminalItem::ensureFont() {
    if (m_builder) {
        return true;
    }
    m_fonts = std::make_unique<render::FontDb>();
    if (!m_fonts->valid()) {
        qWarning("render: DirectWrite unavailable; cannot resolve a font");
        return false;
    }
    // The configured family first, then the built-in candidates. A family that
    // is set but not installed falls back rather than failing: someone syncing
    // one config across machines should get a working terminal on the machine
    // that lacks the font, not an empty window.
    std::string configured;
    if (m_settings != nullptr) {
        configured = m_settings->text("font.family");
    }
    std::optional<std::string> family;
    if (!configured.empty() && m_fonts->firstInstalled(std::array{std::string_view{configured}})) {
        family = configured;
    } else {
        if (!configured.empty()) {
            qWarning("render: font family '%s' is not installed; falling back", configured.c_str());
        }
        family = m_fonts->firstInstalled(kFontCandidates);
    }
    if (!family.has_value()) {
        qWarning("render: none of the candidate monospace families are installed");
        return false;
    }
    m_family = *family;

    const auto spec = m_fonts->resolve(m_family, false, false, m_pxHeight);
    if (!spec.has_value()) {
        qWarning("render: '%s' resolved to no usable font file", m_family.c_str());
        return false;
    }
    m_pool = std::make_unique<render::ShapePool>();
    const auto faceId = m_pool->registerFace(*spec);
    if (!faceId.has_value()) {
        qWarning("render: FreeType could not open %s", spec->path.c_str());
        return false;
    }
    m_primaryFace = *faceId;

    const auto metrics = m_pool->metrics(m_primaryFace);
    if (!metrics.has_value() || metrics->cellWidth <= 0 || metrics->lineHeight <= 0) {
        qWarning("render: degenerate font metrics");
        return false;
    }
    m_atlas = std::make_unique<render::GlyphAtlas>(metrics->cellWidth, metrics->lineHeight);
    m_builder = std::make_unique<render::FrameBuilder>(*metrics, renderTheme());
    m_raster = [this](std::uint32_t face, std::uint32_t glyph, render::GlyphBitmap& out) {
        return m_pool->rasterize(face, glyph, out);
    };
    // Machine-readable on purpose: tools/dpi-check.cmd reads px= and cell= back
    // out, and a family name is a variable number of words, so anything
    // positional breaks the moment the font changes.
    qInfo("render: px=%d cell=%dx%d dpr=%.2f workers=%u family='%s'", m_pxHeight,
          metrics->cellWidth, metrics->lineHeight, m_dpr, m_pool->workerCount(), m_family.c_str());
    return true;
}

void TerminalItem::ensureStarted() {
    if (m_started || m_cols <= 0 || m_rows <= 0 || !m_builder) {
        return;
    }
    m_session = std::make_unique<core::vt::Session>(m_rows, m_cols);
    m_session->onReply = [this](const std::string& reply) {
        // Null during a bench run, which has no backend by design, and between
        // resetSession() and the next start. A dropped answerback is the right
        // outcome in both: there is nothing on the other end to read it.
        if (m_backend == nullptr) {
            return;
        }
        sendInput(QByteArray(reply.data(), static_cast<qsizetype>(reply.size())));
    };
    m_session->onOsc = [this](const core::vt::OscAction& action) { handleOsc(action); };
    if (m_benchFrames > 0) {
        // A bench run needs no shell: the flood is synthesised so the numbers
        // are reproducible rather than at the mercy of PowerShell's output.
        m_started = true;
        qInfo("bench: synthetic flood, %dx%d", m_cols, m_rows);
        // The churn is driven from the GUI thread, by a timer, and NOT from the
        // renderer. Two reasons, both learned the hard way: mutating the grid
        // from render() races the GUI thread (it crashed in release, where the
        // timing stopped hiding it), and QQuickRhiItemRenderer::update() asks
        // only for a re-RENDER, so a renderer-driven flood re-drew one static
        // frame forever and reported a fast number that measured nothing.
        auto* pump = new QTimer(this);  // owned by this
        pump->setInterval(0);
        connect(pump, &QTimer::timeout, this, [this, pump] {
            if (m_benchSteps >= m_benchFrames + 400) {
                pump->stop();  // the renderer reports well before this
                return;
            }
            update();  // QQuickItem::update: marks dirty so synchronize() runs
        });
        pump->start();
        rebuildFrame();
        return;
    }
    if (m_backend == nullptr) {
        // T52: the profile decides, not this class. A default-constructed
        // Profile is a local shell, which is what an unadorned window is.
        adoptBackend(makeBackend(m_profile, m_vault, this));
    }
    if (m_backend->start(m_cols, m_rows)) {
        m_started = true;
        qInfo("terminal started: %dx%d (%s)", m_cols, m_rows,
              session::backendName(m_profile.backend).c_str());
        const QByteArray inject = qgetenv("KRAIT_TERM_INJECT");
        if (!inject.isEmpty()) {
            QTimer::singleShot(1200, this, [this, inject] { sendInput(inject + "\r"); });
        }
    }
}

int TerminalItem::bufferWidth() const {
    // Qt: "texture size = item size * device pixel ratio", unless pinned. This
    // is what renderTarget()->pixelSize() will be, and the shaders divide by it
    // — feeding them the LOGICAL width stretches every glyph on a scaled
    // display, which is the blur rules/render.md forbids.
    if (fixedColorBufferWidth() > 0) {
        return fixedColorBufferWidth();
    }
    return std::max(1, static_cast<int>(std::lround(width() * m_dpr)));
}

int TerminalItem::bufferHeight() const {
    if (fixedColorBufferHeight() > 0) {
        return fixedColorBufferHeight();
    }
    return std::max(1, static_cast<int>(std::lround(height() * m_dpr)));
}

void TerminalItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    updateGrid();
}

void TerminalItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickRhiItem::itemChange(change, value);
    // The only per-monitor DPI hook Qt offers an item: there is no
    // QWindow::devicePixelRatioChanged signal, and QWindow::screenChanged fires
    // only when the window moves, not when the same monitor is rescaled.
    if (change == ItemDevicePixelRatioHasChanged) {
        applyDevicePixelRatio(value.realValue);
    } else if (change == ItemSceneChange && value.window != nullptr) {
        // Startup is not a CHANGE: an item born on a 200% monitor never gets
        // the ratio event, so without this the first font is rasterised at 100%
        // and every glyph is stretched for the life of the window.
        applyDevicePixelRatio(value.window->effectiveDevicePixelRatio());
    }
}

void TerminalItem::setSettings(settings::Registry* registry) {
    m_settings = registry;
    // T73. The panel reads editor.command; it has no other way to reach the
    // registry, and a null one just means the OS default editor.
    m_files->setSettings(registry);
    if (m_settings == nullptr) {
        return;
    }
    // reloaded(), not changed(): a hot reload can move several settings at once
    // and applying them one signal at a time would rebuild the font stack
    // repeatedly and, worse, reflow the grid against a half-applied state.
    connect(m_settings, &settings::Registry::reloaded, this, &TerminalItem::applySettings);
    applySettings();
}

void TerminalItem::applySettings() {
    if (m_settings == nullptr) {
        return;
    }

    // T78. The clear alpha. 1 when there is no background image, so a default
    // install pays nothing for this and asks the scene graph for no blending —
    // and so the setting cannot make a terminal translucent over a window with
    // nothing behind it, which just looks like a rendering bug.
    const bool hasImage = !m_settings->text("background.image").empty();
    const auto opacity = static_cast<float>(m_settings->integer("background.opacity"));
    const float wanted = hasImage ? opacity / 100.0F : 1.0F;
    if (wanted != m_backgroundAlpha) {
        m_backgroundAlpha = wanted;
        // QQuickRhiItem only composites the item's alpha when it is told to,
        // and turning it on unconditionally would cost a blend for every
        // terminal in the default install.
        setAlphaBlending(m_backgroundAlpha < 1.0F);
        update();
    }
    const std::string family = m_settings->text("font.family");
    const int size = static_cast<int>(m_settings->integer("font.size"));
    const bool ligatures = m_settings->boolean("font.ligatures");
    const auto ambiguous = m_settings->text("unicode.eastAsianAmbiguous") == "wide"
                               ? core::unicode::Ambiguous::Wide
                               : core::unicode::Ambiguous::Narrow;
    const auto scrollback = static_cast<int>(m_settings->integer("scrollback.lines"));
    // T70. Read here so a hot reload reaches a tab that is already logging. It
    // takes effect on the NEXT keypress rather than reopening the file: turning
    // it off mid-session must stop recording input immediately, and turning it
    // on must not silently rewrite what is already on disk.
    m_logInput = m_settings->boolean(kLogIncludeInput);

    // The east-asian-ambiguous setting changes how many CELLS existing text
    // occupies, so it is a reflow, not a repaint. Applied to the live grid
    // because every width the grid measures from here reads it (grid.h).
    if (m_session && m_session->grid().ambiguous != ambiguous) {
        m_session->grid().ambiguous = ambiguous;
        if (m_builder) {
            m_builder->invalidate();
        }
    }
    if (m_session) {
        // Two caps, both real: a line cap the user set, and a cell cap that
        // bounds MEMORY. One 100k-column line is not 1/10000th of the budget,
        // so a line count alone does not bound anything (T21).
        constexpr std::size_t kCellsPerLineBudget = 200;
        m_session->grid().scrollback().setCaps(static_cast<std::size_t>(std::max(0, scrollback)),
                                               static_cast<std::size_t>(std::max(0, scrollback)) *
                                                   kCellsPerLineBudget);
    }

    // T68. Switching taskbar progress off has to retract what is already on the
    // button, not just stop the next report: the bar a remote host put there is
    // exactly what the user is declining, and leaving it would make "off" mean
    // "off from now on" — with no way to clear a bar that is already stuck.
    // forget() is the same path a closing tab takes, so the aggregate across
    // the other tabs is recomputed rather than blanked.
    if (!m_settings->boolean("notify.taskbarProgress") && g_taskbar != nullptr) {
        g_taskbar->forget(this);
    }

    // T68. Recompiled on every reload rather than only on connect: turning
    // triggers off, or turning `allowSend` on, has to reach the tabs that are
    // already open — a switch that only applies to the next session is one the
    // user cannot tell apart from a broken one.
    applyTriggers();

    // Against the CONFIGURED family, not the resolved one: ensureFont() writes
    // its fallback into m_family, so comparing that made every hot reload of
    // any unrelated setting look like a font change and tear the whole stack
    // down — whenever font.family is unset (the default) or names a font this
    // machine does not have.
    const bool fontChanged =
        family != m_configuredFamily || size != m_basePxHeight || ligatures != m_ligatures;
    m_configuredFamily = family;
    m_ligatures = ligatures;
    m_basePxHeight = size;
    if (fontChanged) {
        // Same path a DPI change takes: the glyphs are baked at a fixed pixel
        // size, so a new family or size means re-rasterising the whole stack
        // and reflowing the grid to the new cell size.
        const qreal dpr = m_dpr;
        m_dpr = 0.0;  // force applyDevicePixelRatio past its no-change exit
        applyDevicePixelRatio(dpr);
        return;
    }
    rebuildFrame();
    update();
}

void TerminalItem::applyDevicePixelRatio(qreal dpr) {
    if (dpr <= 0.0) {
        return;
    }
    const int px = std::max(4, static_cast<int>(std::lround(m_basePxHeight * dpr)));
    m_dpr = dpr;
    if (m_builder && px == m_pxHeight) {
        // e.g. 100% -> 110%, which rounds to the same font size. The FONT is
        // unchanged, but the colour buffer is not: fall through to updateGrid
        // rather than returning, or the grid and the pty keep the column count
        // derived from the old buffer size. updateGrid has its own no-op exit.
        updateGrid();
        return;
    }
    m_pxHeight = px;
    qInfo("render: device pixel ratio %.2f, font %dpx", dpr, m_pxHeight);
    // A DPI change is a FONT change. Glyphs are baked into the atlas at a fixed
    // pixel size, so anything short of re-rasterising them is a scaled bitmap —
    // exactly the blur "DPI change without restart or blur" rules out. The
    // whole stack goes because the cell metrics it was built from are stale.
    m_builder.reset();
    m_atlas.reset();
    m_pool.reset();
    m_fonts.reset();
    m_runs.clear();
    m_shaped.clear();
    m_faces.clear();
    m_rowRanges.clear();
    m_cols = 0;  // force updateGrid() past its no-change early out
    m_rows = 0;
    updateGrid();
}

void TerminalItem::updateGrid() {
    // ItemSceneChange is delivered from setParentItem during QML construction,
    // BEFORE componentComplete applies `anchors.fill: parent` — so the item is
    // still 0x0 here. Without this guard the grid comes out 2x2 and, because
    // ensureStarted() runs below, PowerShell is spawned into a two-column
    // pseudoconsole: its banner and first prompt are hard-wrapped at 2 columns
    // and that is permanently in scrollback. geometryChange calls back with the
    // real size a moment later.
    if (m_benchCols == 0 && (width() <= 0.0 || height() <= 0.0)) {
        return;
    }
    if (!ensureFont()) {
        return;
    }
    const render::FaceMetrics& metrics = m_builder->metrics();
    // A 4K bench run pins the grid so the workload matches the M0 baseline
    // rather than whatever the window happens to be.
    const int cols = m_benchCols > 0 ? m_benchCols : std::max(2, bufferWidth() / metrics.cellWidth);
    const int rows =
        m_benchRows > 0 ? m_benchRows : std::max(2, bufferHeight() / m_builder->cellHeight());
    if (cols == m_cols && rows == m_rows) {
        return;
    }
    m_cols = cols;
    m_rows = rows;
    if (m_started) {
        // Before resize(), so a mode-2048 report built from the new size cannot
        // be built from a stale cell size — a font change and a window change
        // both arrive here, and the second one changes these.
        m_session->grid().cellWidthPx = metrics.cellWidth;
        m_session->grid().cellHeightPx = m_builder->cellHeight();
        m_session->grid().resize(rows, cols);
        if (m_benchFrames == 0) {
            m_backend->resize(cols, rows);
        }
        // T82. AFTER the pty resize, not before: the spec says a terminal
        // should not report until "the internal resize is complete", and an
        // application that reads the report before SIGWINCH lands would size
        // itself against a pty that has not moved yet.
        reportResize();
        // A resize is one of the three cases where a full rebuild is correct.
        m_builder->invalidate();
    } else {
        ensureStarted();
    }
    rebuildFrame();
    update();
}

void TerminalItem::handleOutput(const QByteArray& bytes) {
    if (!m_session) {
        return;
    }
    m_log.writeOutput(bytes);
    if (m_log.takeFailure()) {
        // The disk filled or the share went away. A per-tab banner, never a
        // dialog, and never the silent stop that is the whole reason this class
        // latches a failure at all — the path stays on the strip so the user can
        // see WHICH file died.
        emit errorRaised(tr("Logging stopped."), m_log.error());
        emit loggingChanged();
    }
    if (m_hexdump) {
        // The DUMP is fed to the parser, not the bytes. Feeding both would put
        // the escape sequences on screen twice — once as hex and once as their
        // effect — and the effect is exactly what a hexdump is for avoiding.
        const std::string dump = formatHexdump(bytes, m_hexdumpOffset);
        m_hexdumpOffset += static_cast<std::uint64_t>(bytes.size());
        m_session->feed({reinterpret_cast<const std::uint8_t*>(dump.data()), dump.size()});
        // The REAL bytes, not the dump: a hexdump is a view, not a mode, so a
        // trigger keeps watching the session rather than watching hex digits.
        runTriggers(bytes);
        rebuildFrame();
        update();
        return;
    }
    m_session->feed({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                     static_cast<std::size_t>(bytes.size())});
    // After the parser, never during it: a trigger can raise a banner, a banner
    // changes the strip height, and that reaches Grid::resize() — resizing the
    // grid under a parser part-way through a chunk is the re-entrancy
    // handleOsc() already defers around.
    runTriggers(bytes);
    rebuildFrame();
    update();
}

// Everything handleOsc() DOES is deferred, and that is the load-bearing part of
// this function rather than an optimisation.
//
// handleOsc runs inside Session::feed(), i.e. with the parser part-way through
// a chunk of remote bytes. Both of its side effects can re-enter the event
// loop: raising a banner gives the strip height, which re-evaluates the
// terminal's own height binding, which reaches geometryChange() and
// Grid::resize() — resizing the grid UNDER the parser that is still writing to
// it. And an STA COM call pumps window messages, which can deliver a socket
// read and start a second feed(). The decision is made here, where the sequence
// order is known; the effect runs once the chunk is done.
// T82/T83. The two notifications an application subscribes to with a mode.
//
// Both are built here rather than in the core for the same reason: one needs a
// pixel size and the other needs a theme, and src/core/ has neither. The core
// owns the SWITCH — grid.inBandResize and grid.paletteUpdates — so an
// application that never asked can never be sent one.

void TerminalItem::reportResize() {
    if (!m_session || !m_session->grid().inBandResize) {
        return;
    }
    const core::vt::Grid& grid = m_session->grid();
    // Height before width in BOTH pairs. See resizeReport() in csi_mode.cpp,
    // which builds the identical string for the enable-time report — the two
    // exist separately because that one has no window and this one has no
    // parser, and a shared helper would have to live in whichever of them the
    // other cannot reach.
    const std::string report = "\x1b[48;" + std::to_string(grid.rows) + ";" +
                               std::to_string(grid.cols) + ";" +
                               std::to_string(grid.rows * grid.cellHeightPx) + ";" +
                               std::to_string(grid.cols * grid.cellWidthPx) + "t";
    sendInput(QByteArray(report.data(), static_cast<qsizetype>(report.size())));
}

void TerminalItem::reportColorScheme() {
    if (!m_session || !m_session->grid().paletteUpdates || g_themes == nullptr) {
        return;
    }
    // CSI ? 997 ; 1 n for dark, ; 2 n for light (contour's spec). The light or
    // dark reading comes from the background's luminance rather than from
    // anything the theme file claims, so a hand-edited file cannot tell an
    // application to pick the unreadable palette.
    const bool dark = theme::isDark(g_themes->current());
    const QByteArray report =
        dark ? QByteArrayLiteral("\x1b[?997;1n") : QByteArrayLiteral("\x1b[?997;2n");
    sendInput(report);
}

// OSC 4/10/11/12 and their resets (T83).
//
// Session-local, always: a remote host changing a colour must not rewrite the
// user's theme file, and the next tab must not inherit what this one was told.
// resetSession() drops the overrides with everything else.
//
// Indices 16-255 are NOT settable, and that is a documented limit rather than
// an oversight. They are the fixed xterm cube — every terminal agrees on them
// and programs COMPUTE a colour from an index there rather than redefining one
// — so honouring them would mean carrying a 256-entry table through every
// frame's theme for a case with no known emitter. A set is ignored and a query
// is answered from the cube, which is the true current value either way.
void TerminalItem::handleColorOsc(const core::vt::OscAction& action) {
    using Kind = core::vt::OscAction::Kind;
    using Slot = core::vt::OscAction::ColorSlot;
    if (!m_builder) {
        return;
    }

    render::Theme live = m_builder->theme();
    const auto slotOf = [&live](Slot slot, int index) -> std::uint32_t* {
        switch (slot) {
        case Slot::Foreground:
            return &live.fg;
        case Slot::Background:
            return &live.bg;
        case Slot::Cursor:
            return &live.cursor;
        case Slot::Palette:
            return index >= 0 && index < 16 ? &live.ansi[static_cast<std::size_t>(index)] : nullptr;
        }
        return nullptr;
    };

    if (action.kind == Kind::ColorQuery) {
        // xterm answers in the same OSC it was asked in, with XParseColor's
        // 16-bit form. Each component is doubled rather than left-shifted:
        // 0xff must answer ffff and not ff00, or white reads as 99.6% white.
        std::uint32_t value = 0;
        if (action.slot == Slot::Palette) {
            value = render::paletteColor(static_cast<std::uint8_t>(action.colorIndex), live);
        } else {
            const std::uint32_t* found = slotOf(action.slot, action.colorIndex);
            value = found != nullptr ? *found : 0;
        }
        char buffer[64];
        const int code = action.slot == Slot::Palette      ? 4
                         : action.slot == Slot::Foreground ? 10
                         : action.slot == Slot::Background ? 11
                                                           : 12;
        const unsigned r = (value >> 16) & 0xFFU;
        const unsigned g = (value >> 8) & 0xFFU;
        const unsigned b = value & 0xFFU;
        const int written = action.slot == Slot::Palette
                                ? std::snprintf(buffer, sizeof(buffer),
                                                "\x1b]4;%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\",
                                                action.colorIndex, r, r, g, g, b, b)
                                : std::snprintf(buffer, sizeof(buffer),
                                                "\x1b]%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\",
                                                code, r, r, g, g, b, b);
        if (written > 0) {
            sendInput(QByteArray(buffer, written));
        }
        return;
    }

    if (action.kind == Kind::ColorReset) {
        // Back to the THEME's value, not to a hardcoded default: "reset" means
        // what the user configured, and a terminal that reset to black would
        // undo a theme the user picked on purpose.
        const render::Theme base = renderTheme();
        if (action.slot == Slot::Palette && action.colorIndex < 0) {
            live.ansi = base.ansi;
        } else if (action.slot == Slot::Palette) {
            if (std::uint32_t* target = slotOf(Slot::Palette, action.colorIndex);
                target != nullptr) {
                *target = base.ansi[static_cast<std::size_t>(action.colorIndex)];
            }
        } else {
            std::uint32_t* target = slotOf(action.slot, -1);
            const std::uint32_t value = action.slot == Slot::Foreground   ? base.fg
                                        : action.slot == Slot::Background ? base.bg
                                                                          : base.cursor;
            if (target != nullptr) {
                *target = value;
            }
        }
    } else {
        const std::optional<theme::Rgb> parsed = theme::parseColor(action.text);
        if (!parsed) {
            return;  // an unreadable spec changes nothing, and says nothing back
        }
        std::uint32_t* target = slotOf(action.slot, action.colorIndex);
        if (target == nullptr) {
            return;  // index 16-255: see the note above
        }
        *target = *parsed;
    }

    m_builder->setTheme(live);
    m_builder->invalidate();
    rebuildFrame();
    update();
}

void TerminalItem::handleOsc(const core::vt::OscAction& action) {
    using Kind = core::vt::OscAction::Kind;

    if (action.kind == Kind::Progress) {
        // T68. Checked HERE rather than at the far end of the post, so a host
        // hammering `\e]9;4;1;1\e\\` costs nothing at all when the user has
        // declined it — the whole point of the switch is that a remote sender
        // cannot make Krait do work on the user's desktop. A bar already on
        // the button is cleared by applySettings(), not by this path, which is
        // never reached again once the setting is off.
        if (m_settings != nullptr && !m_settings->boolean("notify.taskbarProgress")) {
            return;
        }
        // COALESCED, one posted event at a time. TaskbarProgress throttles the
        // COM calls, but a 64 KiB read of `\e]9;4;1;1\e\\` is ~5000 sequences,
        // and posting a QMetaCallEvent for each would allocate 5000 times
        // before the loop drained one of them — the throttle would be bounding
        // the wrong thing. The newest report wins; the intermediate values of a
        // progress bar nobody has seen yet are not worth a heap allocation
        // each. The aggregation, the tie-break between tabs and the cap on how
        // often the taskbar is poked all stay in TaskbarProgress, so a second
        // reporter cannot implement any of them differently.
        m_progress = {action.progress, action.percent};
        if (m_progressPosted) {
            return;
        }
        m_progressPosted = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                m_progressPosted = false;
                if (g_taskbar != nullptr) {
                    g_taskbar->report(this, m_progress.first, m_progress.second);
                }
            },
            Qt::QueuedConnection);
        return;
    }

    if (action.kind == Kind::ColorSet || action.kind == Kind::ColorQuery ||
        action.kind == Kind::ColorReset) {
        handleColorOsc(action);
        return;
    }

    if (action.kind != Kind::PromptMark) {
        return;  // clipboard, title and hyperlinks are not wired yet
    }

    if (action.promptMark == core::vt::kMarkOutputStart) {
        m_commandSince.start();
        return;
    }
    if (action.promptMark != core::vt::kMarkCommandEnd || !m_commandSince.isValid()) {
        return;
    }
    const qint64 elapsedMs = m_commandSince.elapsed();
    m_commandSince.invalidate();  // one notification per command, not per D

    if (m_settings == nullptr || !m_settings->boolean("notify.longCommand")) {
        return;
    }
    const qint64 thresholdMs = m_settings->integer("notify.longCommandSeconds") * 1000;
    if (elapsedMs < thresholdMs) {
        return;
    }
    // Focused means the user is already watching this window; telling them what
    // they can see is how a notification becomes something people turn off.
    // focusWindow() rather than QWindow::isActive(), which is also true for
    // every window in the same transient-parent chain.
    QWindow* const own = window();
    if (own == nullptr || QGuiApplication::focusWindow() == own) {
        return;
    }

    const QString took = QString::number(elapsedMs / 1000);
    const QString message =
        action.exitCode > 0
            ? tr("A command failed after %1 s (exit status %2).").arg(took).arg(action.exitCode)
            : tr("A command finished after %1 s.").arg(took);
    const QString detail = sessionTitle();

    QMetaObject::invokeMethod(
        this,
        [this, message, detail] {
            // Re-read rather than captured: the window can be gone by the time
            // this runs, and alert() on a stale pointer is a crash from a
            // notification nobody would be there to see.
            if (QWindow* const now = window(); now != nullptr) {
                // The platform's own "this window wants you" affordance: on
                // Windows it flashes the taskbar button until the window is
                // activated. Not requestActivate(), which STEALS focus — the
                // user is deliberately somewhere else.
                now->alert(0);
                // T68. All THREE surfaces fire, and they are complementary
                // rather than redundant: the flash is for someone still
                // looking at the taskbar, the balloon reaches someone in
                // another window entirely, and the banner below is the only
                // one that survives being missed. `notify.longCommand` gates
                // all of them together — it was checked before this was
                // posted.
                if (g_notifier != nullptr) {
                    // The session title heads the balloon so it says WHICH tab
                    // even from outside the app; an empty one falls back to
                    // the application name inside notify().
                    g_notifier->notify(now, detail, message);
                }
            }
            emit commandFinished(message, detail);
        },
        Qt::QueuedConnection);
}

bool TerminalItem::jumpToPrompt(int direction) {
    if (!m_session) {
        return false;
    }
    core::vt::Grid& grid = m_session->grid();
    if (grid.onAlternateScreen()) {
        // markPrompt refuses to place marks here for the same reason: the
        // scrollback is the NORMAL buffer's history, so jumping would scroll
        // another buffer's text over a full-screen application's viewport —
        // which vim would then repaint away on the next keystroke, leaving the
        // binding looking broken rather than refused.
        return false;
    }
    const std::size_t from = grid.viewTopLine();
    const std::optional<std::size_t> found =
        direction < 0 ? grid.prevPrompt(from) : grid.nextPrompt(from);
    if (!found) {
        return false;
    }
    grid.scrollToLine(*found);
    rebuildFrame();
    update();
    return true;
}

void TerminalItem::rebuildFrame() {
    if (!m_session || !m_builder) {
        return;
    }
    core::vt::Grid& grid = m_session->grid();

    // viewportRows() is the scrolled-back-aware view (T21), NOT the raw screen:
    // a reader scrolled up must see history, not the live bottom.
    m_viewport = grid.viewportRows();

    // T68's highlight action. Re-derived from the VISIBLE rows every frame
    // rather than recorded where a match landed: a coordinate captured at feed
    // time is invalidated by the next reflow, which is the scrollback landmine
    // CLAUDE.md names, and it would also miss text scrolled back into view.
    //
    // Reuses the search path's lineText() — same text extraction, same column
    // mapping — because two notions of "what is on this row" is how a highlight
    // ends up one cell off from the characters it is meant to be under.
    //
    // ponytail: one regex pass per visible row per rebuild, bounded by
    // TriggerEngine::kMaxScan per row and skipped entirely when no rule asks for
    // a highlight. If a profile with many highlight rules ever costs a visible
    // frame, the upgrade is to cache per row against the damage list the way
    // FrameBuilder already caches its instances.
    m_highlights.clear();
    if (m_triggers.hasHighlight()) {
        for (std::size_t row = 0; row < m_viewport.size(); ++row) {
            const std::string text =
                core::vt::lineText(m_viewport[row], grid.clusters(), &m_highlightColumns);
            m_triggers.highlightRanges(text, m_highlightRanges);
            for (const auto& [begin, end] : m_highlightRanges) {
                if (end >= m_highlightColumns.size()) {
                    continue;  // a range past the row's text has no cells
                }
                m_highlights.push_back({.row = static_cast<int>(row),
                                        .beginCol = m_highlightColumns[begin],
                                        .endCol = m_highlightColumns[end]});
            }
        }
    }

    render::FrameParams params;
    params.cols = grid.cols;
    params.highlights = m_highlights;
    params.selection = m_selection;
    params.cursor.visible = grid.viewOffset() == 0;  // hidden while scrolled back
    params.cursor.focused = hasActiveFocus();
    params.cursor.row = grid.row;
    params.cursor.col = grid.col;
    params.cursor.style = render::CursorStyle::Block;
    if (m_copyMode) {
        // Copy mode's cursor REPLACES the shell's: two blocks on screen with
        // only one of them answering the keyboard is the ambiguity the mode
        // indicator exists to remove, and the shell's is hidden anyway the
        // moment the viewport scrolls back.
        params.cursor.visible = true;
        params.cursor.row = m_copyCursor.row;
        params.cursor.col = m_copyCursor.col;
    }

    // ONE shaping batch for the whole frame, not one per row. FrameBuilder's
    // callback is per row, so shaping inside it would mean a separate blocking
    // round trip to the worker pool for every damaged row — 63 waits a frame on
    // a full screen, which is what "coalesce damage" in rules/render.md exists
    // to prevent. So: split every row that will be rebuilt, shape the lot in one
    // call, then hand build() a slice per row.
    m_runs.clear();
    m_rowRanges.assign(m_viewport.size(), {0, 0});
    for (int row = 0; row < static_cast<int>(m_viewport.size()); ++row) {
        if (!m_builder->rowNeedsRebuild(row, grid.damage)) {
            continue;
        }
        const auto begin = m_runs.size();
        render::splitRow(m_viewport[static_cast<std::size_t>(row)].cells, grid.clusters(), row,
                         m_runs);
        m_rowRanges[static_cast<std::size_t>(row)] = {begin, m_runs.size() - begin};
    }
    const auto shapeTimeout =
        m_benchFrames > 0 ? std::chrono::milliseconds{1000} : std::chrono::milliseconds{8};
    m_faces = render::shapeWithFallback(*m_pool, *m_fonts, m_runs, m_primaryFace, m_family,
                                        m_pxHeight, m_ligatures, m_shaped, shapeTimeout);

    m_builder->build(m_viewport, grid.damage, params, m_raster, *m_atlas, [&](int row) {
        const auto range = m_rowRanges[static_cast<std::size_t>(row)];
        return render::FrameBuilder::RowRuns{
            .runs = std::span(m_runs).subspan(range.first, range.second),
            .shaped = std::span(m_shaped).subspan(range.first, range.second),
            .faces = std::span(m_faces).subspan(range.first, range.second),
        };
    });
    grid.damage.clear();

    m_frame.solids.assign(m_builder->solids().begin(), m_builder->solids().end());
    m_frame.glyphs.assign(m_builder->glyphs().begin(), m_builder->glyphs().end());
    appendComposition();
    m_frame.atlasWidth = m_atlas->width();
    m_frame.atlasHeight = m_atlas->height();
    // ACCUMULATE, do not overwrite. rebuildFrame() runs once per pty chunk and
    // several chunks land per presented frame, so only the last one's dirty
    // range would otherwise reach synchronize(): a glyph first rasterised in an
    // earlier rebuild, with nothing new in the last, was never uploaded.
    // synchronize() clears these after it takes the pixels.
    m_frame.atlasDirtyTop = std::min(m_frame.atlasDirtyTop, m_atlas->dirtyTop());
    m_frame.atlasDirtyBottom = std::max(m_frame.atlasDirtyBottom, m_atlas->dirtyBottom());
    if (m_atlas->takeGrew()) {
        m_frame.atlasGrew = true;
        // Growth doubles the atlas HEIGHT, so every normalised v coordinate
        // already cached in a row is now half the value it should be. The rows
        // that were not redrawn this frame would sample the wrong part of the
        // texture until something else happened to touch them.
        m_builder->invalidate();
    }
    render::unpackColor(m_builder->theme().bg, m_frame.clearR, m_frame.clearG, m_frame.clearB);
    m_frame.clearA = m_backgroundAlpha;
    m_atlas->clearDirty();
}

const std::vector<std::uint8_t>* TerminalItem::atlasPixels() const {
    return m_atlas ? &m_atlas->pixels() : nullptr;
}

void TerminalItem::clearAtlasDirty() {
    m_frame.atlasDirtyTop = std::numeric_limits<int>::max();
    m_frame.atlasDirtyBottom = 0;
    m_frame.atlasGrew = false;
}

void TerminalItem::stepBench(int frame) {
    if (!m_session) {
        return;
    }
    ++m_benchSteps;
    if (m_benchSteps % 100 == 0) {
        // Evidence that the flood actually ran, and how much of it the shaped-run
        // cache absorbed. A bench whose churn silently stopped would otherwise
        // report a fast frame that measured nothing.
        qInfo("bench: step %d, rows %d, cache hits %llu misses %llu, rowsRebuilt %d", m_benchSteps,
              m_session->grid().rows, static_cast<unsigned long long>(m_pool->cacheHits()),
              static_cast<unsigned long long>(m_pool->cacheMisses()), m_builder->rowsRebuilt());
    }
    core::vt::Grid& grid = m_session->grid();
    // Every cell changes every frame: the worst realistic frame a terminal
    // produces, and the same workload shape the M0 baseline used.
    for (int row = 0; row < grid.rows; ++row) {
        grid.cursorSet(row, 0);
        for (int col = 0; col < grid.cols; ++col) {
            grid.putChar(static_cast<char32_t>(0x21 + ((row + col + frame) % 94)));
        }
    }
    grid.damage.markAll();
    rebuildFrame();
}

void TerminalItem::dumpAtlas(const QString& path) const {
    if (!m_atlas) {
        return;
    }
    const QImage image(m_atlas->pixels().data(), m_atlas->width(), m_atlas->height(),
                       m_atlas->width(), QImage::Format_Grayscale8);
    if (!image.copy().save(path)) {
        qWarning("render: cannot write atlas dump %s", qPrintable(path));
    }
}

void TerminalItem::finishBench(const QString& reportJson) {
    qInfo("bench: %s", qPrintable(reportJson));
    const QByteArray out = qgetenv("KRAIT_BENCH_OUT");
    if (!out.isEmpty()) {
        QFile file(QString::fromLocal8Bit(out));
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning("bench: cannot write %s", out.constData());
            QCoreApplication::exit(1);  // never let stale JSON pass as fresh
            return;
        }
        file.write(reportJson.toUtf8());
    }
    const QByteArray dump = qgetenv("KRAIT_ATLAS_DUMP");
    if (!dump.isEmpty()) {
        dumpAtlas(QString::fromLocal8Bit(dump));
    }
    QCoreApplication::quit();
}

void TerminalItem::focusInEvent(QFocusEvent* event) {
    QQuickRhiItem::focusInEvent(event);
    rebuildFrame();  // the cursor changes from an outline to a filled block
    update();
}

void TerminalItem::focusOutEvent(QFocusEvent* event) {
    QQuickRhiItem::focusOutEvent(event);
    rebuildFrame();
    update();
}

void TerminalItem::cellAt(const QPointF& pos, int& row, int& col) const {
    if (!m_builder) {
        row = 0;
        col = 0;
        return;
    }
    const int cellW = std::max(1, m_builder->metrics().cellWidth);
    const int cellH = std::max(1, m_builder->cellHeight());
    // The event is in LOGICAL pixels; the cell metrics are in device pixels.
    // Without the ratio a click lands on the wrong cell on any scaled display.
    const int x = static_cast<int>(pos.x() * m_dpr);
    const int y = static_cast<int>(pos.y() * m_dpr);
    col = std::clamp(x / cellW, 0, std::max(0, m_cols - 1));
    row = std::clamp(y / cellH, 0, std::max(0, m_rows - 1));
}

bool TerminalItem::reportMouse(input::MouseAction action, Qt::MouseButton button,
                               Qt::MouseButtons buttonsDown, Qt::KeyboardModifiers mods,
                               const QPointF& pos, int wheelSteps) {
    if (!m_session || !m_started) {
        return false;
    }
    int row = 0;
    int col = 0;
    cellAt(pos, row, col);
    const QByteArray report = input::encodeMouse(input::MouseEvent{.action = action,
                                                                   .button = button,
                                                                   .buttonsDown = buttonsDown,
                                                                   .mods = mods,
                                                                   .row = row,
                                                                   .col = col,
                                                                   .wheelSteps = wheelSteps},
                                                 m_session->grid());
    if (report.isEmpty()) {
        return false;  // not tracked, or not expressible: the event stays ours
    }
    sendInput(report);
    return true;
}

void TerminalItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && m_started) {
        // Middle-click paste goes through the SAME guard as Ctrl+Shift+V. A
        // second, unguarded paste path is how paste-guards get bypassed.
        paste();
        event->accept();
        return;
    }
    // Shift is the universal override: it lets a user select text inside a
    // full-screen application that has grabbed the mouse. Without it there is
    // no way to copy from vim or htop.
    if (!event->modifiers().testFlag(Qt::ShiftModifier) &&
        reportMouse(input::MouseAction::Press, event->button(), event->buttons(),
                    event->modifiers(), event->position(), 0)) {
        forceActiveFocus();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QQuickRhiItem::mousePressEvent(event);
        return;
    }
    int row = 0;
    int col = 0;
    cellAt(event->position(), row, col);
    m_selection = {
        .active = false, .anchorRow = row, .anchorCol = col, .cursorRow = row, .cursorCol = col};
    m_dragging = true;
    forceActiveFocus();
    rebuildFrame();
    update();
    event->accept();
}

void TerminalItem::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        if (reportMouse(input::MouseAction::Move, Qt::NoButton, event->buttons(),
                        event->modifiers(), event->position(), 0)) {
            event->accept();
            return;
        }
        QQuickRhiItem::mouseMoveEvent(event);
        return;
    }
    int row = 0;
    int col = 0;
    cellAt(event->position(), row, col);
    // A press alone is not a selection — only a drag off the anchor is, or a
    // single click would wipe the previous selection and highlight one cell.
    m_selection.cursorRow = row;
    m_selection.cursorCol = col;
    m_selection.active = row != m_selection.anchorRow || col != m_selection.anchorCol;
    rebuildFrame();
    update();
    event->accept();
}

void TerminalItem::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_dragging && reportMouse(input::MouseAction::Release, event->button(), event->buttons(),
                                   event->modifiers(), event->position(), 0)) {
        event->accept();
        return;
    }
    const bool wasDragging = m_dragging;
    m_dragging = false;
    // Copy-on-select, like every X terminal: a selection that needs a menu to
    // reach is a selection nobody uses.
    if (wasDragging && m_selection.active) {
        copySelection();
    }
    QQuickRhiItem::mouseReleaseEvent(event);
}

void TerminalItem::wheelEvent(QWheelEvent* event) {
    if (event->angleDelta().y() == 0) {
        // A tilt wheel or a horizontal trackpad gesture. Treating it as a
        // vertical delta made every horizontal flick scroll DOWN and send a
        // button-5 report with it.
        QQuickRhiItem::wheelEvent(event);
        return;
    }
    const int steps = event->angleDelta().y() > 0 ? 1 : -1;
    if (reportMouse(input::MouseAction::Press, Qt::NoButton, event->buttons(), event->modifiers(),
                    event->position(), steps)) {
        event->accept();
        return;
    }
    if (m_session) {
        // Not tracked: the wheel scrolls OUR viewport. Three rows a notch is
        // what the platform reports as one detent.
        m_session->grid().scrollView(steps * 3);
        rebuildFrame();
        update();
        event->accept();
        return;
    }
    QQuickRhiItem::wheelEvent(event);
}

void TerminalItem::keyPressEvent(QKeyEvent* event) {
    if (!m_started || !m_session) {
        QQuickRhiItem::keyPressEvent(event);
        return;
    }
    // Ctrl+Shift+C copies rather than sending ^C. Checked before translation
    // because the terminal would otherwise swallow it as ETX.
    if (event->matches(QKeySequence::Paste) ||
        (event->modifiers().testFlag(Qt::ControlModifier) &&
         event->modifiers().testFlag(Qt::ShiftModifier) && event->key() == Qt::Key_V)) {
        paste();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Copy) ||
        (event->modifiers().testFlag(Qt::ControlModifier) &&
         event->modifiers().testFlag(Qt::ShiftModifier) && event->key() == Qt::Key_C)) {
        copySelection();
        event->accept();
        return;
    }
    // T71. AFTER copy/paste and BEFORE translateKey: copy mode owns bare
    // letters, so it has to come before translation, and it must come before
    // the scrollViewToBottom() below — snapping to the live screen on every
    // keypress is exactly what a mode for reading history must not do.
    if (m_copyMode) {
        if (handleCopyKey(event)) {
            event->accept();
        } else {
            QQuickRhiItem::keyPressEvent(event);
        }
        return;
    }

    const core::vt::Grid& grid = m_session->grid();
    const QByteArray bytes = input::translateKey(
        event->key(), event->modifiers(), event->text(),
        input::KeyModes{.appCursorKeys = grid.appCursorKeys, .kittyFlags = grid.kittyKeys.flags});
    if (bytes.isEmpty()) {
        // Not ours. Leaving it unaccepted is what lets the QML chrome see it.
        QQuickRhiItem::keyPressEvent(event);
        return;
    }
    // Any keypress snaps the viewport back to the live screen and drops the
    // selection, which is what every terminal does.
    m_session->grid().scrollViewToBottom();
    m_selection.active = false;
    sendInput(bytes);
    rebuildFrame();
    update();
    event->accept();
}

void TerminalItem::appendComposition() {
    if (!m_composition.active() || !m_builder || !m_session) {
        return;
    }
    const core::vt::Grid& grid = m_session->grid();
    const render::FaceMetrics& metrics = m_builder->metrics();
    const int columns = m_composition.columns(grid.ambiguous);
    const int cells = render::preeditCells(grid.col, columns, m_cols);
    if (cells <= 0) {
        return;
    }
    const render::CellRect area =
        render::preeditRect(metrics, grid.row, grid.col, columns, m_cols, m_rows);

    // Background first: the composition covers whatever the grid has in those
    // cells, and drawing the preedit glyphs over live text without clearing it
    // leaves both readable and neither legible.
    const render::Theme& theme = m_builder->theme();
    render::SolidInstance background{.x = static_cast<float>(area.x),
                                     .y = static_cast<float>(area.y),
                                     .w = static_cast<float>(area.w),
                                     .h = static_cast<float>(area.h)};
    render::unpackColor(theme.selectionBg, background.r, background.g, background.b);
    m_frame.solids.push_back(background);

    // The underline every platform's IME convention uses to say "not committed
    // yet". Without it a preedit is indistinguishable from typed text, and the
    // user cannot tell what Enter is about to do.
    const int thickness = std::max(1, metrics.lineHeight / 12);
    render::SolidInstance underline{.x = static_cast<float>(area.x),
                                    .y = static_cast<float>(area.y + area.h - thickness),
                                    .w = static_cast<float>(area.w),
                                    .h = static_cast<float>(thickness)};
    render::unpackColor(theme.fg, underline.r, underline.g, underline.b);
    m_frame.solids.push_back(underline);

    // The preedit text itself, shaped through the same pool as everything else
    // — a second shaping path is how a composition ends up looking different
    // from the text it commits to.
    render::Run run;
    run.row = grid.row;
    run.col = grid.col;
    int column = grid.col;
    for (const std::u32string& cluster : m_composition.clusters()) {
        const int cellsUsed = core::unicode::clusterWidth(cluster, grid.ambiguous);
        render::ClusterRef ref;
        ref.col = column;
        ref.cells = static_cast<std::uint8_t>(std::max(1, cellsUsed));
        ref.offset = static_cast<std::uint32_t>(run.text.size());
        ref.len = static_cast<std::uint8_t>(cluster.size());
        run.text += cluster;
        run.clusters.push_back(ref);
        // A zero-width mark shares its base's column, which is what puts a Thai
        // tone mark over the vowel rather than in the next cell.
        column += cellsUsed;
    }
    if (run.clusters.empty()) {
        return;
    }
    std::vector<render::Run> runs{std::move(run)};
    std::vector<render::ShapedRun> shaped;
    const auto faces =
        render::shapeWithFallback(*m_pool, *m_fonts, runs, m_primaryFace, m_family, m_pxHeight,
                                  m_ligatures, shaped, std::chrono::milliseconds{8});
    m_builder->appendShapedRun(runs.front(), shaped.front(), faces.front(), theme.fg, m_raster,
                               *m_atlas, m_frame.glyphs);
    // Deliberately NO atlas bookkeeping here. rebuildFrame() reads the dirty
    // range and takeGrew() immediately after this returns; doing it here as
    // well consumed both, so an active composition reported a clean atlas and
    // suppressed the upload of every glyph rasterised that frame — its own
    // preedit included.
}

void TerminalItem::sendPaste(const QByteArray& bytes) {
    if (bytes.isEmpty() || !m_started) {
        return;
    }
    // A paste, like a keypress, snaps the viewport back to the live screen.
    m_session->grid().scrollViewToBottom();
    m_selection.active = false;
    sendInput(bytes);
    rebuildFrame();
    update();
}

void TerminalItem::paste() {
    if (!m_session || !m_started) {
        return;
    }
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) {
        return;
    }
    // Capped before anything walks it. rules/net.md: the clipboard is remote
    // input, and a web page can put 100 MB on it — preparePaste makes several
    // copies and runs eight regexes over the lot, on the UI thread. 1 MB is
    // far past any command a person pastes and far short of a freeze.
    constexpr qsizetype kMaxPasteChars = 1 << 20;
    bool truncated = false;
    if (text.size() > kMaxPasteChars) {
        qWarning("paste: clipboard held %lld characters; truncated to %lld",
                 static_cast<long long>(text.size()), static_cast<long long>(kMaxPasteChars));
        text.truncate(kMaxPasteChars);
        truncated = true;
    }
    auto guarded = input::preparePaste(text, m_session->grid().bracketedPaste);
    if (truncated) {
        // Never send a truncated paste without asking: half a command is its
        // own hazard, and the user has to know they are not sending what they
        // copied.
        guarded.risk = input::PasteRisk::Multiline;
        guarded.sanitised = true;
    }
    if (!guarded.needsConfirm()) {
        sendPaste(guarded.bytes);
        return;
    }
    // Hold the SANITISED bytes, not the clipboard text: the clipboard can
    // change between the banner appearing and the user answering, and re-reading
    // it on "allow" would send something the banner never described.
    m_pendingPaste = guarded.bytes;
    QString detail = text.section(QChar::LineFeed, 0, 0).trimmed();
    constexpr int kDetailChars = 120;
    if (detail.size() > kDetailChars) {
        detail = detail.left(kDetailChars) + QStringLiteral("...");
    }
    emit pasteConfirmRequested(input::describeRisk(guarded.risk), detail);
}

void TerminalItem::resolvePaste(bool allow) {
    const QByteArray pending = std::exchange(m_pendingPaste, QByteArray{});
    if (allow) {
        sendPaste(pending);
    }
}

void TerminalItem::copySelection() {
    if (!m_session || !m_selection.active) {
        return;
    }
    const std::string text =
        render::selectionText(m_viewport, m_selection, m_session->grid().clusters());
    if (text.empty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(
        QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
}

void TerminalItem::inputMethodEvent(QInputMethodEvent* event) {
    // m_builder too: applyDevicePixelRatio() resets it and updateGrid() can
    // return before ensureFont() rebuilds it (zero geometry, or no usable
    // font), all while m_started stays true. Composition arriving in that
    // window would dereference null.
    if (!m_started || !m_session || !m_builder) {
        QQuickRhiItem::inputMethodEvent(event);
        return;
    }
    // A commit is finished text and goes down the same path as a keystroke.
    // The preedit does NOT: it belongs to the IME until it commits, so it never
    // reaches the parser and never lands in scrollback.
    const QString commit = event->commitString();
    if (!commit.isEmpty()) {
        m_composition.clear();
        m_session->grid().scrollViewToBottom();
        sendInput(commit.toUtf8());
    }

    auto cursorInChars = static_cast<int>(event->preeditString().size());
    for (const QInputMethodEvent::Attribute& attr : event->attributes()) {
        if (attr.type == QInputMethodEvent::Cursor) {
            cursorInChars = attr.start;
        }
    }
    m_composition.setPreedit(event->preeditString(), cursorInChars);

    // The composition sits on cells the grid still owns, so those rows have to
    // be redrawn when it changes — including when it disappears.
    m_builder->invalidate();
    rebuildFrame();
    update();
    // Tells the IME its anchor moved. Skipping this is why candidate windows
    // stay parked where the composition STARTED.
    updateInputMethod(Qt::ImCursorRectangle | Qt::ImAnchorRectangle);
    event->accept();
}

QVariant TerminalItem::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled:
        return m_started;
    case Qt::ImHints:
        // No autocorrect, no predictive text, no capitalisation: a shell is not
        // a message box, and an IME that "helps" corrupts commands.
        return static_cast<int>(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText |
                                Qt::ImhMultiLine);
    case Qt::ImCursorRectangle:
    case Qt::ImAnchorRectangle: {
        if (!m_builder || !m_session) {
            return {};
        }
        const core::vt::Grid& grid = m_session->grid();
        // The candidate window anchors at the CARET inside the composition, not
        // at its start — the IME draws its list under the character being
        // converted.
        const int caret = m_composition.active() ? m_composition.cursorColumns(grid.ambiguous) : 0;
        const render::CellRect rect =
            render::cursorRect(m_builder->metrics(), grid.row, grid.col + caret, m_cols, m_rows);
        // Back to LOGICAL coordinates: the metrics are device pixels, and Qt
        // wants item coordinates. On a 200% display, handing over device pixels
        // puts the candidate window at twice the offset — off the bottom-right
        // of the window for anything but the first row.
        const qreal scale = m_dpr > 0.0 ? m_dpr : 1.0;
        return QRectF(rect.x / scale, rect.y / scale, rect.w / scale, rect.h / scale);
    }
    case Qt::ImFont:
        return QFont(QString::fromStdString(m_family), m_basePxHeight);
    case Qt::ImSurroundingText:
        // Deliberately empty. An IME uses surrounding text for reconversion,
        // and handing it the screen contents would leak whatever is on it —
        // including output from a command the user did not type.
        return QString();
    default:
        break;
    }
    return QQuickRhiItem::inputMethodQuery(query);
}

QQuickRhiItemRenderer* TerminalItem::createRenderer() {
    return new TerminalRenderer;  // owned by the scene graph node
}

void TerminalRenderer::synchronize(QQuickRhiItem* item) {
    auto* term = static_cast<TerminalItem*>(item);
    m_item = term;
    m_benchFrames = term->benchFrames();
    // Churn HERE, not in render(): synchronize() is the one phase Qt runs with
    // the GUI thread blocked, so this cannot race it — and it runs exactly once
    // per frame the GUI thread actually produced, which is what makes the frame
    // count and the churn count the same number.
    if (m_benchFrames > 0 && !m_benchDone) {
        term->stepBench(m_frameIndex);
        constexpr int kWarmup = 60;
        if (m_frameIndex == kWarmup) {
            m_timer.start();
        } else if (m_frameIndex > kWarmup) {
            m_cpuMs.push_back(static_cast<double>(m_timer.nsecsElapsed()) / 1e6);
            m_timer.restart();
        }
        ++m_frameIndex;
    }
    m_frame = term->frame();  // a copy: the render thread must not walk the grid
    if (const std::vector<std::uint8_t>* pixels = term->atlasPixels()) {
        // Hand over only when something actually changed. A steady-state frame
        // touches no new glyph and so uploads nothing.
        // The size check is not redundant with atlasGrew: the atlas can grow
        // twice between two presented frames, and takeGrew() consumes the flag
        // on the first rebuild, so the frame that gets here reports the new
        // height with atlasGrew false. Without this the GPU keeps the old,
        // smaller buffer and the upload can never complete.
        if (m_frame.atlasGrew || m_frame.atlasDirtyBottom > m_frame.atlasDirtyTop ||
            !m_gpu.hasAtlas() || pixels->size() != m_gpu.atlasBytes()) {
            m_gpu.setAtlasPixels(*pixels);
        }
        // Consumed HERE, not in rebuildFrame(): this is the point the pixels
        // actually reach the render thread, so it is the only point at which
        // "everything dirty has been handed over" is true. Clearing it per
        // rebuild is what lost the ranges of every rebuild but the last.
        term->clearAtlasDirty();
    }
}

void TerminalRenderer::initialize(QRhiCommandBuffer* cb) {
    if (!m_shadersLoaded) {
        // Loaded once and injected: GpuResources never touches the resource
        // system, which is what lets a plain test drive it.
        m_gpu.setShaders({
            .solidVert = loadShader(":/shaders/cell.vert.qsb"),
            .solidFrag = loadShader(":/shaders/cell.frag.qsb"),
            .glyphVert = loadShader(":/shaders/glyph.vert.qsb"),
            .glyphFrag = loadShader(":/shaders/glyph.frag.qsb"),
        });
        m_shadersLoaded = true;
    }
    m_gpu.sync(rhi(), renderTarget()->renderPassDescriptor(), renderTarget()->sampleCount(), cb,
               m_frame, renderTarget()->pixelSize());
}

void TerminalRenderer::render(QRhiCommandBuffer* cb) {
    // No render pass is active here, which is what sync() needs for its
    // uploads. A changed rhi() IS the device-lost path: Qt tears the scene
    // graph down on FrameOpDeviceLost and comes back with a new QRhi, and
    // GpuResources drops every resource and rebuilds against it.
    const bool ok =
        m_gpu.sync(rhi(), renderTarget()->renderPassDescriptor(), renderTarget()->sampleCount(), cb,
                   m_frame, renderTarget()->pixelSize());
    // T78. The alpha is what lets a background image through, and it is applied
    // HERE rather than by drawing the image on top at low opacity — that
    // version washes out the text as well as the background, which is how this
    // feature makes a terminal unreadable.
    const QColor clear =
        QColor::fromRgbF(m_frame.clearR, m_frame.clearG, m_frame.clearB, m_frame.clearA);
    if (!ok) {
        // Still clear the target: a skipped pass shows whatever the last device
        // left in it, which after a device loss is garbage.
        cb->beginPass(renderTarget(), clear, {1.0F, 0});
        cb->endPass();
        return;
    }

    if (m_benchFrames > 0 && !m_benchDone) {
        const double gpuSeconds = cb->lastCompletedGpuTime();
        if (gpuSeconds > 0.0) {
            m_gpuMs.push_back(gpuSeconds * 1000.0);
        }
        if (static_cast<int>(m_cpuMs.size()) >= m_benchFrames) {
            m_benchDone = true;
            reportBench();
        }
    }

    const QSize outputSize = renderTarget()->pixelSize();
    cb->beginPass(renderTarget(), clear, {1.0F, 0});
    cb->setViewport({0.0F, 0.0F, static_cast<float>(outputSize.width()),
                     static_cast<float>(outputSize.height())});
    m_gpu.draw(cb, m_frame);
    cb->endPass();
    // Deliberately NO update() here for the bench. The renderer driving itself
    // advances render() without a synchronize(), so the flood would re-draw one
    // static frame at thousands of fps and report a number that measured
    // nothing. The GUI-thread pump in ensureStarted() is the only driver.
}

void TerminalRenderer::reportBench() {
    std::vector<double> cpu = m_cpuMs;
    std::ranges::sort(cpu);
    const double avg =
        std::accumulate(cpu.begin(), cpu.end(), 0.0) / static_cast<double>(cpu.size());
    const double p99 = cpu[cpu.size() * 99 / 100];
    const double worst = cpu.back();
    const double fps = avg > 0.0 ? 1000.0 / avg : 0.0;
    double gpuAvg = 0.0;
    if (!m_gpuMs.empty()) {
        gpuAvg = std::accumulate(m_gpuMs.begin(), m_gpuMs.end(), 0.0) /
                 static_cast<double>(m_gpuMs.size());
    }
    const QSize size = renderTarget()->pixelSize();
    const QString json = QString::asprintf(
        "{\"target\":\"%dx%d\",\"frames\":%d,\"cpu_avg_ms\":%.3f,\"cpu_p99_ms\":%.3f,"
        "\"cpu_max_ms\":%.3f,\"fps\":%.1f,\"gpu_avg_ms\":%.3f,\"gpu_samples\":%d,"
        "\"solids\":%d,\"glyphs\":%d}",
        size.width(), size.height(), static_cast<int>(m_cpuMs.size()), avg, p99, worst, fps, gpuAvg,
        static_cast<int>(m_gpuMs.size()), static_cast<int>(m_frame.solids.size()),
        static_cast<int>(m_frame.glyphs.size()));
    // Queued: the item lives on the GUI thread and outlives the renderer.
    QMetaObject::invokeMethod(m_item, "finishBench", Qt::QueuedConnection, Q_ARG(QString, json));
}

}  // namespace krait::app
