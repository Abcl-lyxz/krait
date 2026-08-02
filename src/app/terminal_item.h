#pragma once

#include "../net/conpty/conpty_backend.h"
#include "../net/ibackend.h"
#include "../net/vault/vault.h"
#include "capture.h"
#include "core/terminal/session.h"
#include "input/copy_mode.h"
#include "input/ime.h"
#include "input/mouse.h"
#include "input/paste.h"
#include "render/atlas/glyph_atlas.h"
#include "render/frame_builder.h"
#include "render/gpu_resources.h"
#include "render/ime_metrics.h"
#include "render/shaper/fontdb.h"
#include "render/shaper/shape_pool.h"
#include "session/profile.h"
#include "session/triggers.h"
#include "settings/registry.h"
#include "sftp_model.h"
#include "taskbar_progress.h"
#include <rhi/qrhi.h>

#include <QElapsedTimer>
#include <QQuickRhiItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace krait::app {

// Forward-declared rather than included: only a pointer crosses this header,
// and notifier.h drags in nothing this file wants (cpp.md: forward-declare in
// headers where possible).
class Notifier;

namespace theme {
// Same reason as Notifier above: only a pointer crosses this header, and
// theme/store.h reaches QtCore containers this file has no use for.
class ThemeStore;
}  // namespace theme

// The real terminal view (plan T25): ConPTY -> core Session -> run splitting ->
// the HarfBuzz shaper pool -> the glyph atlas -> two instanced QRhi draws.
// Replaces the M0 spike path, which rendered ASCII from a fixed 95-cell strip
// and could only show one CJK character as '?'.
//
// Two pipelines, not one: solid rectangles (backgrounds, selection, cursor,
// underlines) and textured glyph quads. A glyph is NOT a cell — it carries the
// shaper's offsets and the font's bearings — so the spike's
// one-instance-per-cell model could never place a Thai mark or a ligature.
class TerminalItem : public QQuickRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TerminalView)

    // The tab strip binds to these. Properties rather than plain getters
    // because a tab opened as "Shell" and then pointed at a prod host through
    // the palette has to relabel itself, and a binding is the only thing that
    // does that without the strip polling.
    Q_PROPERTY(QString sessionTitle READ sessionTitle NOTIFY sessionChanged)
    Q_PROPERTY(QString sessionAccent READ sessionAccent NOTIFY sessionChanged)
    // The tunnel pane's model: one row per configured forward, with its live
    // state. Empty for every backend that has no tunnels, which is all of them
    // except SSH.
    Q_PROPERTY(QVariantList tunnels READ tunnels NOTIFY tunnelsChanged)
    // T65. The SFTP panel's view-model. Never null — it exists for every
    // backend and reports available == false for the ones that cannot transfer
    // files, so QML has one thing to ask rather than a null check plus a check.
    Q_PROPERTY(krait::app::SftpModel* files READ files CONSTANT)
    // T69. The snippet bar's model: one row per snippet this profile defines,
    // {name, preview}. Empty for a profile that defines none, which is what
    // keeps the strip from appearing on tabs it has nothing to say about.
    Q_PROPERTY(QVariantList snippets READ snippets NOTIFY sessionChanged)
    // T70. Properties rather than the Q_INVOKABLE getters they replace: the
    // status strip BINDS to these, and a logging session that stopped without
    // the strip noticing is exactly the failure this task exists to prevent.
    Q_PROPERTY(bool logging READ loggingEnabled NOTIFY loggingChanged)
    Q_PROPERTY(QString logPath READ logPath NOTIFY loggingChanged)
    // T71. Whether keys are going to copy mode instead of the shell. The user
    // needs to know why their typing stopped reaching the far end.
    Q_PROPERTY(bool copyMode READ copyModeActive NOTIFY copyModeChanged)

  public:
    TerminalItem();
    ~TerminalItem() override;

    QQuickRhiItemRenderer* createRenderer() override;

    // Snapshot handed to the renderer in synchronize(). Copied rather than
    // shared: the render thread must not walk the grid while the GUI thread is
    // feeding bytes into it.
    const render::FrameData& frame() const { return m_frame; }

    // The atlas pixels, for the renderer to upload. Borrowed, valid until the
    // next rebuildFrame() on the GUI thread — synchronize() runs with the GUI
    // thread blocked, which is the whole reason that is safe.
    const std::vector<std::uint8_t>* atlasPixels() const;

    // Marks the accumulated atlas dirty range as handed over. Called from
    // synchronize(), which runs with the GUI thread blocked — the one place
    // where touching item state from the render thread is safe.
    void clearAtlasDirty();

    int benchFrames() const { return m_benchFrames; }

    // Called (queued) from the renderer when a flood bench completes.
    Q_INVOKABLE void finishBench(const QString& reportJson);

    // One bench frame's worth of grid churn, driven from the renderer so the
    // flood runs at presentation rate — the same way the M0 baseline was taken.
    Q_INVOKABLE void stepBench(int frame);

    // Dumps the atlas to a PNG for the golden-image gate.
    Q_INVOKABLE void dumpAtlas(const QString& path) const;

    // The three things every terminal borrows, handed over once by main()
    // BEFORE the QML engine builds anything (T53).
    //
    // Static because QML constructs terminals dynamically now — opening a tab
    // creates one — and there is no hook that runs between "QML made an item"
    // and "the item needs its dependencies". The findChildren() sweep this
    // replaces could only ever reach the items that existed at startup.
    //
    // All three are borrowed and outlive every terminal: main() owns them, so
    // each tab reads the same live settings (a hot reload reaches all of them),
    // writes to the same vault, and sees the same session list.
    static void setServices(settings::Registry* registry, net::Vault* vault,
                            session::ProfileStore* store);

    // T75. Borrowed; main() owns it. Same argument as setServices: QML builds
    // terminals on demand, so a theme handed over once at startup would only
    // reach the tabs that existed then.
    static void setThemes(theme::ThemeStore* themes);

    // The one taskbar button every tab's OSC 9;4 reports collapse onto (T67).
    // Borrowed and owned by main(), like the three above; null in the tests and
    // in a bench run, which have no window and therefore no button.
    static void setTaskbar(TaskbarProgress* taskbar);

    // The one notification-area icon every tab's long-command balloon goes out
    // through (T68). Borrowed and owned by main(); null in the tests, which
    // have no window to hang a shell icon on.
    static void setNotifier(Notifier* notifier);

    // Hands over the settings registry (T31) for a terminal built outside the
    // normal path — the tests. setServices() covers the app.
    void setSettings(settings::Registry* registry);

    // Paste, guarded (T28). Reads the clipboard, sanitises it, and either sends
    // it or raises pasteConfirmRequested. QML calls this from the paste action.
    Q_INVOKABLE void paste();

    // Answers a pending confirmation. `allow` false discards the paste.
    Q_INVOKABLE void resolvePaste(bool allow);

    // The secret store every SSH backend borrows (T52). Borrowed; main() owns
    // it so all tabs share one file rather than each holding its own copy of a
    // vault that the others' writes would then overwrite.
    void setVault(net::Vault* vault);

    // Opens `profile` here (T52), replacing whatever this terminal was running.
    // Safe before the first frame — the backend is built when the grid size is
    // known — and safe afterwards, which is what the palette needs.
    //
    // T53 gives the palette a new TAB instead; this stays the way one terminal
    // is pointed at one profile, so only the caller changes.
    void openProfile(const session::Profile& profile);

    // The same by id, for QML — which is where a tab is opened from and which
    // cannot carry a Profile. False when the store has no such session; the
    // caller decides what to say about it.
    Q_INVOKABLE bool openProfileById(const QString& profileId);

    // What to put on this terminal's tab: the profile name, or a plain "Shell"
    // for an unnamed local one.
    QString sessionTitle() const;

    // rules/ui.md: safety accents (prod = red) are a core UX invariant. Empty
    // means the theme decides.
    QString sessionAccent() const;

    const QVariantList& tunnels() const { return m_tunnels; }

    SftpModel* files() const { return m_files; }

    const QVariantList& snippets() const { return m_snippets; }

    // T69. Sends snippet `index` to the session. Goes through preparePaste()
    // like every other block of text entering a pty — a snippet is user-authored
    // and therefore trusted in a way trigger-matched remote text is not, but it
    // must not become the one path that skips the ESC/C0 strip that path does.
    // Out-of-range is a no-op: QML holds an index into a list the profile can
    // replace under it.
    Q_INVOKABLE void sendSnippet(int index);

    // T74. One broadcast line, through the same preparePaste() path as every
    // other block of text entering a pty. FALSE means this session could not
    // take it — no backend, not started yet, or the shell has exited — and the
    // broadcast drops it from the target set rather than swallowing the line.
    //
    // Called by name through QMetaObject::invokeMethod (see broadcast.h), so
    // the signature here is the contract: renaming it or changing its
    // parameters breaks the fan-out at runtime, not at compile time.
    Q_INVOKABLE bool sendBroadcast(const QString& text);

    // Answers hostKeyPromptRequested. Ignored when the session is not SSH or
    // has moved on, so a banner answered late cannot reach a different backend.
    Q_INVOKABLE void respondHostKey(bool trust);

    // Answers credentialPromptRequested. `remember` stores it in the vault.
    Q_INVOKABLE void respondCredential(const QString& text, bool remember);

    // T57. Shows the bytes arriving rather than what they mean — the question
    // "what did the device ACTUALLY send" is one a terminal cannot otherwise
    // answer. Input still goes out unchanged: this is a view, not a mode.
    Q_INVOKABLE void setHexdump(bool on);

    Q_INVOKABLE bool hexdumpEnabled() const { return m_hexdump; }

    // Starts or stops capturing the session to a file named by the
    // `logging.pathTemplate` setting. Returns the path, or empty when stopping
    // or on failure — the caller puts it in a banner, because a log nobody can
    // find is a log nobody trusts.
    Q_INVOKABLE QString toggleLogging();

    bool loggingEnabled() const { return m_log.isOpen(); }

    const QString& logPath() const { return m_logPath; }

    // T71. Enters or leaves copy mode. While it is on, keys drive the cursor
    // over the scrollback instead of reaching the shell, and the viewport stops
    // being snapped back to the live screen by every keypress.
    Q_INVOKABLE void setCopyMode(bool on);

    bool copyModeActive() const { return m_copyMode; }

    // T67, jump-to-prompt. `direction` is -1 for the previous OSC 133 prompt
    // mark and +1 for the next one, counted from the row at the top of the
    // viewport so pressing it twice walks two prompts rather than returning to
    // the same one. False when there is no prompt that way — the caller says so
    // in a banner rather than leaving a shortcut that silently does nothing.
    Q_INVOKABLE bool jumpToPrompt(int direction);

    // Raises a banner from outside the backend path — the command line naming a
    // session that does not exist, and nothing else so far. Exists because
    // rules/ui.md makes the per-tab banner the ONLY error surface, so a caller
    // with no backend still needs a way in.
    Q_INVOKABLE void raiseError(const QString& message, const QString& hint);

    // What the command line asked for, consumed by the FIRST item constructed.
    //
    // Static because there is no hook between "QML constructed the item" and
    // "the item has geometry", and geometry is what triggers the first start.
    // Without this, `krait prod` spawns the default PowerShell during
    // loadFromModule() and main() then kills it a few lines later — paying a
    // process create and a wait on the UI thread to show nothing.
    static void setLaunchProfile(const session::Profile& profile);

  signals:
    // The tab strip's label needs to change when the terminal is pointed at a
    // different session. Without it a tab opened as "Shell" keeps saying so
    // after the palette turns it into a prod connection.
    void sessionChanged();
    void tunnelsChanged();

    // rules/ui.md: a per-tab banner, never an app-modal dialog. `detail` is the
    // first line of what would be sent, so the user can see what they are
    // agreeing to without leaving the terminal.
    void pasteConfirmRequested(const QString& message, const QString& detail);

    // A backend failure, already mapped to what the user reads (T33). Per-tab
    // and never modal: rules/ui.md bans app-modal surfaces in session flows.
    void errorRaised(const QString& message, const QString& hint);

    // The SSH host key needs a human (T52). `askable` false means there is
    // nothing to accept — a changed key is refused whatever the answer is
    // (rules/net.md), and the banner shows no Trust button at all rather than
    // one that does nothing.
    void hostKeyPromptRequested(const QString& message, const QString& detail, bool askable);

    // A password, passphrase or keyboard-interactive answer is needed. `prompt`
    // is SERVER-CONTROLLED for keyboard-interactive, so the banner renders it
    // as plain text. `echo` false means a password field.
    void credentialPromptRequested(const QString& prompt, bool echo);

    // Connection progress worth putting on screen: connected, or reconnecting
    // with the attempt count. Empty message clears it.
    void connectionNotice(const QString& message);

    // T67. A command marked by OSC 133 ran longer than the configured
    // threshold and finished while the window was NOT focused. A banner, not a
    // dialog — rules/ui.md bans app-modal surfaces in session flows, and this
    // one arrives precisely when the user is doing something else.
    void commandFinished(const QString& message, const QString& detail);

    // T70/T71. State the tab strip shows for as long as it lasts. A log running
    // unnoticed is how a secret ends up on disk, and a mode that has swallowed
    // the keyboard without saying so reads as a hung terminal.
    void loggingChanged();
    void copyModeChanged();

    // T68. A trigger matched. Its own signal rather than reusing the one above:
    // the two arrive for different reasons and a handler that wanted to treat
    // them differently would have nothing to branch on. `message` already
    // contains sanitised remote text.
    void triggerMatched(const QString& message);

  protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    // ItemDevicePixelRatioHasChanged: the only hook Qt gives for a per-monitor
    // DPI change (there is no QWindow::devicePixelRatioChanged signal).
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    // T29. inputMethodQuery answers WHERE the candidate window goes; without it
    // the IME guesses, and on Windows that means the top-left of the screen.
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

  private:
    // T75. Pushes the store's current palette into the frame builder and forces
    // a full rebuild. Connected to ThemeStore::changed, so it runs for a theme
    // switch, a live-editor keystroke and a theme file edited on disk alike.
    void applyTheme();
    // T82. Sends `CSI 48 ; rows ; cols ; hpx ; wpx t` when mode 2048 is on, and
    // nothing otherwise. Called after the pty resize, never before: the spec
    // says not to report until the internal resize is complete, and an
    // application reading it before SIGWINCH would size against a stale pty.
    void reportResize();
    // T83. Sends `CSI ? 997 ; 1|2 n` when mode 2031 is on. Light or dark comes
    // from the background's luminance, not from anything a theme file claims.
    void reportColorScheme();
    // T83. OSC 4/10/11/12 and their resets. Session-local by construction: a
    // remote host must not rewrite the user's theme file, and the next tab must
    // not inherit what this one was told.
    void handleColorOsc(const core::vt::OscAction& action);
    void handleOutput(const QByteArray& bytes);
    // T68. Runs the profile's triggers over one chunk of output and carries out
    // whatever fired. Called from handleOutput, after the parser has seen the
    // bytes — the actions can raise a banner, and a banner changes the strip
    // height, which reaches Grid::resize(); doing that mid-parse is the same
    // re-entrancy handleOsc() defers around.
    void runTriggers(const QByteArray& bytes);
    // Rebuilds the compiled trigger set from the current profile and settings.
    void applyTriggers();
    // Appends one line to the trigger log. Opened on first use and left open.
    void logTrigger(const QString& text);
    // Acts on what the core decided an OSC string asked for. Only OSC 9;4 and
    // OSC 133 reach here so far; the clipboard and title kinds are still the
    // core's honest silence (docs/conformance.md).
    void handleOsc(const core::vt::OscAction& action);
    void ensureStarted();
    // Takes ownership of `backend` and wires the IBackend contract plus, when
    // it is an SSH one, the prompts. One place, so a fifth backend cannot
    // arrive with four of the five connections made.
    void adoptBackend(net::IBackend* backend);
    // Drops the current backend and the parsed session, leaving the item ready
    // for ensureStarted() to build the next one.
    void resetSession();
    // Every path that sends bytes to the far end goes through here. One guard
    // instead of five: the item is fully usable with no backend (a bench run,
    // and the window between resetSession() and the next start), and a keypress
    // arriving then must be dropped rather than dereferenced.
    void sendInput(const QByteArray& bytes);
    bool ensureFont();
    void rebuildFrame();
    // Re-rasterises the font stack at `dpr` and reflows the grid. A DPI change
    // is a font change: the glyphs are baked at a fixed pixel size, so anything
    // short of re-rasterising them is a scaled bitmap, i.e. blur.
    void applyDevicePixelRatio(qreal dpr);
    // Pulls every wired setting out of the registry and applies it. Called once
    // at startup and again on each hot reload.
    void applySettings();
    // Re-derives the grid from the colour buffer size and the cell metrics.
    // Shared by a resize and a DPI change, which differ only in what moved.
    void updateGrid();
    // The colour buffer's size in device pixels — what the shaders divide by.
    int bufferWidth() const;
    int bufferHeight() const;
    // Viewport row/col under a widget-space point, clamped into the grid.
    void cellAt(const QPointF& pos, int& row, int& col) const;
    // Sends a mouse report if the application asked for one. Returns whether it
    // did — false means the event is still OURS (selection, viewport scroll).
    bool reportMouse(input::MouseAction action, Qt::MouseButton button,
                     Qt::MouseButtons buttonsDown, Qt::KeyboardModifiers mods, const QPointF& pos,
                     int wheelSteps);
    // Puts the current selection on the clipboard. No-op without one.
    void copySelection();
    // T71. Runs one key through copy mode. False means copy mode does not claim
    // the key, so the caller must leave the event unaccepted and let the QML
    // chrome see it — a mode that ate Ctrl+Shift+P would be a trap.
    bool handleCopyKey(QKeyEvent* event);
    // What copy mode's cursor and anchor currently select.
    render::Selection copySelectionRange() const;
    // Sends already-sanitised paste bytes and snaps the viewport back.
    void sendPaste(const QByteArray& bytes);
    // Appends the in-flight composition to the frame. A preedit is not grid
    // content — it belongs to the IME until it commits — so it is drawn OVER
    // the frame rather than written into the grid.
    void appendComposition();

    std::unique_ptr<render::FontDb> m_fonts;
    std::unique_ptr<render::ShapePool> m_pool;
    std::unique_ptr<render::GlyphAtlas> m_atlas;
    std::unique_ptr<render::FrameBuilder> m_builder;
    std::unique_ptr<core::vt::Session> m_session;
    settings::Registry* m_settings = nullptr;  // borrowed; owned by main()
    // The seam, not a concrete backend: M2 swaps a session profile's SSH
    // backend in here without this class knowing which protocol it drives.
    net::IBackend* m_backend = nullptr;        // owned by this (QObject parent)
    net::Vault* m_vault = nullptr;             // borrowed; owned by main()
    session::ProfileStore* m_store = nullptr;  // borrowed; owned by main()
    // The tunnel pane's rows, mirrored from the SSH backend. Empty for every
    // backend that has no tunnels, which is all of them except SSH.
    QVariantList m_tunnels;
    // T65. Owned by this (QObject parent), so QML holding the pointer through
    // the `files` property cannot outlive or delete it.
    SftpModel* m_files = nullptr;
    // T68/T69. Compiled from m_profile on every openProfile() and on every
    // settings reload; empty when the profile defines none, which is the case
    // that has to cost nothing.
    session::TriggerEngine m_triggers;
    std::vector<session::Snippet> m_snippetList;
    QVariantList m_snippets;
    // Monotonic, for the send rate limiter. Started once and never restarted:
    // an elapsed() that resets would hand every trigger a fresh token bucket.
    QElapsedTimer m_triggerClock;
    std::ofstream m_triggerLog;
    // Latched. Without it a log path that cannot be opened costs a path
    // resolution, an mkpath, an open and a banner on EVERY match — at a rate
    // the remote side sets.
    bool m_triggerLogFailed = false;
    // Where plainText() was when the last chunk ran out. A remote chooses where
    // its writes are cut, so a stripper that restarts at Ground every read can
    // be walked through with a sequence split across two packets.
    session::StripState m_stripState = session::StripState::Ground;
    // The highlight rectangles for the CURRENT frame, in viewport coordinates.
    // Rebuilt from the visible rows each time rather than remembered, so reflow
    // cannot leave one pointing at text that moved.
    std::vector<render::HighlightSpan> m_highlights;
    std::vector<std::pair<std::size_t, std::size_t>> m_highlightRanges;
    std::vector<int> m_highlightColumns;
    // What this terminal is pointed at. Default-constructed = a local shell,
    // which is what a window opened with no arguments should be.
    session::Profile m_profile;

    // T57. The offset is the position in the STREAM, so a hexdump line can be
    // matched against a packet capture; restarting it per read would make the
    // column decorative.
    bool m_hexdump = false;
    std::uint64_t m_hexdumpOffset = 0;
    SessionLog m_log;
    // Remembered separately from m_log: the strip keeps naming the file after a
    // write failure closed the stream, which is the moment the user most needs
    // to know which file stopped.
    QString m_logPath;
    // `logging.includeInput`, pulled in applySettings(). Cached rather than read
    // per keypress, and re-read on hot reload like every other wired setting.
    bool m_logInput = false;

    // T71. Copy mode: the cursor, the anchor, and whether keys are ours.
    bool m_copyMode = false;
    input::CopyCursor m_copyCursor;

    render::RasterFn m_raster;
    std::string m_family;            // what ensureFont() actually resolved
    std::string m_configuredFamily;  // what the settings asked for
    std::uint32_t m_primaryFace = 0;
    // Logical (DPI-independent) font size; m_pxHeight is that scaled to the
    // current device pixel ratio, and is what FreeType actually rasterises at.
    int m_basePxHeight = 20;
    int m_pxHeight = 20;
    qreal m_dpr = 1.0;
    bool m_ligatures = false;

    // Scratch reused across frames so a steady-state frame allocates little.
    std::vector<render::Run> m_runs;
    std::vector<render::ShapedRun> m_shaped;
    std::vector<std::uint32_t> m_faces;
    std::vector<core::vt::Line> m_viewport;
    // Which slice of m_runs belongs to each viewport row: {offset, count}. Rows
    // that were not rebuilt this frame keep {0, 0}.
    std::vector<std::pair<std::size_t, std::size_t>> m_rowRanges;

    // T67. Started by OSC 133 ; C (the command began producing output) and read
    // by ; D. Invalid means no command is running, which is also what a D with
    // no matching C leaves it as — so a shell that only ever sends D can never
    // produce a notification claiming an invented duration.
    QElapsedTimer m_commandSince;

    // The newest OSC 9;4 report, and whether a queued delivery for it is
    // already in flight. Coalescing here is what makes TaskbarProgress's own
    // rate cap describe the real cost — see handleOsc().
    std::pair<core::vt::OscAction::Progress, int> m_progress{core::vt::OscAction::Progress::Remove,
                                                             -1};
    bool m_progressPosted = false;

    render::FrameData m_frame;
    render::Selection m_selection;
    input::Composition m_composition;
    // A paste held back pending confirmation. Already sanitised — what is
    // stored is exactly what will be sent, so an "allow" cannot re-run the
    // guard against different text than the banner described.
    QByteArray m_pendingPaste;
    bool m_dragging = false;
    int m_cols = 0;
    int m_rows = 0;
    bool m_started = false;
    // T74. The far end ran `exit`. m_backend stays non-null after that — the
    // object is alive, it just has nowhere to write — so it is not on its own a
    // usable answer to "can this session take input", which is the question a
    // broadcast has to get right before it reports a line as delivered.
    bool m_exited = false;
    // T74. The connection dropped and the backend is retrying. Neither
    // m_started nor m_exited covers this: m_started is only cleared by
    // resetSession() and m_exited only by a real EOF, so a tab showing
    // "Reconnecting in 5 s" otherwise looks fully alive — and a broadcast
    // would count it as delivered while the bytes were dropped on the floor or
    // queued to be replayed into a FRESH shell later.
    //
    // ponytail: fed only by the SSH reconnect signals, because they are the
    // only ones TerminalItem wires at all — telnet, raw and serial declare
    // `reconnecting` too, but nothing in the app listens to any of them yet.
    // When that gap is closed, set this there as well.
    bool m_reconnecting = false;
    int m_benchFrames = 0;
    int m_benchSteps = 0;
    // Set only by a 4K bench run, to pin the grid to the M0 baseline's 240x63.
    int m_benchCols = 0;
    int m_benchRows = 0;
};

class TerminalRenderer : public QQuickRhiItemRenderer {
  protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

  private:
    void reportBench();

    // Every GPU object lives here (T26), so the device-lost reset is one
    // testable place rather than a block inside a class Qt Quick constructs.
    render::GpuResources m_gpu;
    bool m_shadersLoaded = false;

    render::FrameData m_frame;

    TerminalItem* m_item = nullptr;  // borrowed via synchronize; outlives us
    int m_benchFrames = 0;
    int m_frameIndex = 0;
    bool m_benchDone = false;
    QElapsedTimer m_timer;
    std::vector<double> m_cpuMs;
    std::vector<double> m_gpuMs;
};

}  // namespace krait::app
