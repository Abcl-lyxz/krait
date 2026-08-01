#pragma once

#include "input/paste.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <vector>

class QTimer;

namespace krait::app {

namespace settings {
class Registry;
}

// Type once, send to many (plan T74).
//
// THE INTERLOCK IS THE FEATURE. Broadcasting `rm -rf /` to twelve production
// hosts because a mode was still on is the accident this exists to prevent, so
// the state machine below is the design, not a safety bolt-on around it:
//
//   Off    Nothing is a target and nothing fans out. The strip is not on
//          screen. This is the only state in which typing behaves normally.
//   Ready  Targets are chosen and NAMED ON SCREEN, and nothing is sent. Every
//          way out of Active lands here rather than in Off, because throwing
//          away a twelve-host selection is what teaches people to leave the
//          mode armed instead.
//   Active The line composed in the strip goes to every target on Enter.
//
// Three decisions, each with a reason:
//
//  - It SURVIVES tab switches. That is the feature — watching one host's output
//    while typing to all of them is the whole job. It is safe only because the
//    strip lives on every tab, so there is no tab you can be looking at that
//    does not name the targets.
//  - It does NOT survive the window losing focus: Active drops to Ready.
//    Alt-tabbing away is the strongest signal available that the user's
//    attention left this task, and coming back is exactly the moment the mode
//    is forgotten. Dropping to Ready fails safe (keystrokes reach one session,
//    never more) and costs one keystroke to undo with the selection intact.
//  - It TIMES OUT while idle, back to Ready, after `broadcast.idleSeconds`. The
//    focus rule does not cover the case where the user never leaves the window
//    and simply reads a log for ten minutes; that is the same accident with the
//    window still in front. A real timer rather than a check on the next
//    keystroke: a strip that still says "Active" after it has expired is a
//    stale reassurance, which is worse than no strip.
//
// Nothing here reaches a session directly. Delivery is
// QMetaObject::invokeMethod(tab, "sendBroadcast", ...) — the same
// cross-a-layer-without-a-header mechanism main.cpp already uses for
// runSelfTest(). It keeps this file free of terminal_item.h (which drags in
// QQuickRhiItem and the whole render stack) and it is what lets the fan-out
// rule be tested against a plain QObject, with no GPU, window or live pty.
enum class BroadcastState : std::uint8_t { Off, Ready, Active };

class BroadcastModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int state READ stateValue NOTIFY stateChanged)
    // One row per open tab: { label, marked }. The strip renders this, which is
    // what makes "which sessions am I typing to" answerable at a glance.
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int markedCount READ markedCount NOTIFY tabsChanged)
    // Bumped on every change to the set. isMarked() is a method, so a QML
    // binding registers no dependency on it; reading `revision` first is what
    // makes `(broadcast.revision, broadcast.isMarked(x))` re-evaluate. The same
    // trick SessionPane.qml uses for `(panes.count, repeater.itemAt(...))`.
    Q_PROPERTY(int revision READ revision NOTIFY tabsChanged)
    // Bumped once per line that actually went out. The strip binds to it to
    // know when to clear its input — a bound counter rather than a
    // Connections block, because `Connections.target` is a QObject* and the
    // conversion the QML compiler generates for one assigned from a `var`
    // trips /W4 /WX inside Qt's own headers (unreachable code in the
    // if-constexpr chain of QJSEngine::fromVariant).
    Q_PROPERTY(int sentCount READ sentCount NOTIFY delivered)

  public:
    explicit BroadcastModel(QObject* parent = nullptr);  // owned by parent
    ~BroadcastModel() override;

    // The live settings, borrowed; main() owns them. Null means the defaults.
    void setSettings(settings::Registry* registry);

    // Register or relabel a tab. Called by every SessionPane as it appears and
    // whenever its title changes, so the strip names sessions the way the tab
    // strip does rather than by an index nobody can map back.
    Q_INVOKABLE void offer(QObject* tab, const QString& label);

    // A closed tab stops being a target. Without this a send would fan out to a
    // dangling pointer, and QPointer alone is not enough — a target that has
    // silently become null is one the user still believes is receiving.
    Q_INVOKABLE void forget(QObject* tab);

    Q_INVOKABLE void toggleAt(int index);
    Q_INVOKABLE bool isMarked(QObject* tab) const;

    // Off -> Ready, with `tab` marked. Opening the strip pre-selects the tab it
    // was opened from: a broadcast to nothing is not a state worth offering.
    Q_INVOKABLE void begin(QObject* tab);

    // Ready -> Active. Refused with a report when nothing is marked.
    Q_INVOKABLE void arm();

    // Active -> Ready. Focus loss, the idle timer, and the user's own toggle.
    Q_INVOKABLE void pause();

    // -> Off, marks cleared.
    Q_INVOKABLE void stop();

    // The one way text reaches the sessions. Classifies `line` first: a
    // destructive-looking one is HELD and confirmRequested is emitted, every
    // other line goes straight out. See the note on the implementation for why
    // it is content-triggered rather than a confirmation on every Enter.
    Q_INVOKABLE void send(const QString& line);

    // Answers a held line. `allow` false discards it.
    Q_INVOKABLE void resolve(bool allow);

    int stateValue() const { return static_cast<int>(m_state); }

    BroadcastState state() const { return m_state; }

    const QVariantList& tabs() const { return m_rows; }

    int markedCount() const;

    int revision() const { return m_revision; }

    int sentCount() const { return m_sentCount; }

  signals:
    void stateChanged();
    void tabsChanged();

    // rules/ui.md: a per-tab banner, never a dialog. `detail` is the line
    // itself, so the user can read what they are agreeing to.
    void confirmRequested(const QString& message, const QString& detail);

    // The line went out. The strip clears its input on this and not before, so
    // a held line stays editable while the banner is up.
    void delivered(int count);

    // Something the user has to know but need not answer: targets dropped
    // because they were not connected, the idle timeout firing, an arm with
    // nothing selected.
    void reported(const QString& message, const QString& detail);

  private:
    struct Target {
        // QPointer, not a raw borrow: a tab can be destroyed by anything from a
        // middle click to the window closing, and this list outlives none of it.
        QPointer<QObject> tab;
        QString label;
        bool marked = false;
    };

    void setState(BroadcastState state);
    // The sentence for whatever is currently held. One place, so the banner
    // raised by send() and the one re-raised on a second Enter cannot differ.
    QString describePending() const;
    void rebuildRows();
    void restartIdleTimer();
    // Delivers to every marked target, dropping the ones that refuse.
    void fanOut(const QString& line);

    std::vector<Target> m_targets;
    QVariantList m_rows;
    BroadcastState m_state = BroadcastState::Off;
    int m_revision = 0;
    int m_sentCount = 0;
    // A line held pending confirmation. Held HERE rather than left in the
    // strip's text field for the same reason paste() holds sanitised bytes: the
    // field is editable while the banner is up, and an "allow" must send what
    // the banner described and nothing else.
    QString m_pending;
    input::PasteRisk m_pendingRisk = input::PasteRisk::None;
    settings::Registry* m_settings = nullptr;  // borrowed; owned by main()
    QTimer* m_idle = nullptr;                  // owned by this (QObject parent)
};

}  // namespace krait::app
