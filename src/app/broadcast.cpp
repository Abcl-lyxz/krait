#include "broadcast.h"

#include "input/paste.h"
#include "settings/registry.h"

#include <QGuiApplication>
#include <QTimer>
#include <QVariantMap>

#include <utility>

namespace krait::app {

namespace {

// Used when no registry has been handed over — the tests, and the window
// between construction and main()'s wiring. Same value as the schema default,
// which is the only place it is user-visible.
constexpr int kDefaultIdleSeconds = 300;

}  // namespace

BroadcastModel::BroadcastModel(QObject* parent)
    : QObject(parent), m_idle(new QTimer(this)) {  // owned by this (QObject parent)
    m_idle->setSingleShot(true);
    connect(m_idle, &QTimer::timeout, this, [this] {
        if (m_state != BroadcastState::Active) {
            return;
        }
        pause();
        emit reported(tr("Broadcast paused: nothing was sent for a while."),
                      tr("Press Ctrl+Enter in the broadcast strip to start it again. The wait is "
                         "the broadcast.idleSeconds setting."));
    });

    // The focus rule, in one place. qobject_cast rather than qApp: the unit
    // tests run with no QGuiApplication at all, and a model that could only be
    // constructed under one would be a model that could not be tested.
    if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance()); gui != nullptr) {
        connect(gui, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
                    if (state == Qt::ApplicationActive || m_state != BroadcastState::Active) {
                        return;
                    }
                    pause();
                    emit reported(tr("Broadcast paused: Krait is no longer the active window."),
                                  tr("The sessions you picked are still selected. Press Ctrl+Enter "
                                     "in the broadcast strip to start it again."));
                });
    }
}

BroadcastModel::~BroadcastModel() = default;

void BroadcastModel::setSettings(settings::Registry* registry) {
    m_settings = registry;
    restartIdleTimer();
}

void BroadcastModel::offer(QObject* tab, const QString& label) {
    if (tab == nullptr) {
        return;
    }
    for (Target& target : m_targets) {
        if (target.tab == tab) {
            if (target.label == label) {
                return;  // a relabel that changes nothing must not churn bindings
            }
            target.label = label;
            rebuildRows();
            return;
        }
    }
    m_targets.push_back(Target{.tab = tab, .label = label, .marked = false});
    rebuildRows();
}

void BroadcastModel::forget(QObject* tab) {
    const auto before = m_targets.size();
    std::erase_if(m_targets, [tab](const Target& target) {
        // Null pointers go with it. A target whose tab has already been
        // destroyed is one the user still believes is receiving, which is the
        // one thing this feature must never allow.
        return target.tab == tab || target.tab.isNull();
    });
    if (m_targets.size() == before) {
        return;
    }
    rebuildRows();
    // Only while ARMED. In Ready with nothing ticked yet, markedCount() is
    // legitimately 0 and closing any unrelated pane would otherwise announce
    // that "every session it was sending to has closed" — about a broadcast
    // that was never sending to anything.
    if (m_state == BroadcastState::Active && markedCount() == 0) {
        stop();
        emit reported(tr("Broadcast stopped: every session it was sending to has closed."),
                      QString());
    }
}

void BroadcastModel::toggleAt(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_targets.size()) {
        return;
    }
    m_targets[static_cast<std::size_t>(index)].marked =
        !m_targets[static_cast<std::size_t>(index)].marked;
    rebuildRows();
    // Unticking the last target while armed is the same situation as the last
    // one disconnecting: there is nothing left to broadcast to, so the mode is
    // over rather than silently armed at nobody.
    if (m_state == BroadcastState::Active && markedCount() == 0) {
        pause();
    }
}

bool BroadcastModel::isMarked(QObject* tab) const {
    for (const Target& target : m_targets) {
        if (target.tab == tab) {
            return target.marked;
        }
    }
    return false;
}

int BroadcastModel::markedCount() const {
    int count = 0;
    for (const Target& target : m_targets) {
        if (target.marked && !target.tab.isNull()) {
            ++count;
        }
    }
    return count;
}

void BroadcastModel::begin(QObject* tab) {
    if (m_state == BroadcastState::Off) {
        for (Target& target : m_targets) {
            target.marked = target.tab == tab;
        }
    }
    setState(BroadcastState::Ready);
    rebuildRows();
}

void BroadcastModel::arm() {
    if (m_state == BroadcastState::Off) {
        return;
    }
    if (markedCount() == 0) {
        emit reported(tr("Pick at least one session before starting the broadcast."), QString());
        return;
    }
    setState(BroadcastState::Active);
}

void BroadcastModel::pause() {
    if (m_state != BroadcastState::Active) {
        return;
    }
    // Ready, never Off. Throwing away a twelve-host selection every time the
    // user alt-tabs is what teaches people to leave the mode armed instead.
    setState(BroadcastState::Ready);
}

void BroadcastModel::stop() {
    m_pending.clear();
    for (Target& target : m_targets) {
        target.marked = false;
    }
    setState(BroadcastState::Off);
    rebuildRows();
}

void BroadcastModel::send(const QString& line) {
    if (m_state != BroadcastState::Active) {
        return;
    }
    // THE DESTRUCTIVE-COMMAND DECISION. Confirming every Enter would make the
    // feature useless and train people to click through the banner, which is
    // how the guard stops protecting anyone; confirming nothing is the accident
    // this whole class exists to prevent. So the confirmation is triggered by
    // the CONTENT, and by the same classifier the paste guard already uses —
    // "destructive-looking" means one thing in Krait, not two that drift.
    //
    // Ceiling, stated: the classifier only sees the line typed HERE. A command
    // recalled with the up-arrow inside the far-end shell is not text Krait
    // ever saw, so it is not checked. Closing that would mean parsing every
    // host's echo, which is a different project.
    // ONE held line, ever. Without this, a second send() while the first
    // banner was still up overwrote m_pending — and since the strip is on
    // every tab and the banner is per-tab, accepting the banner that showed X
    // would fan out Y. A confirmation that runs something other than what it
    // described is worse than no confirmation at all.
    //
    // Re-emitted rather than refused, so the banner comes BACK if it was
    // dismissed or replaced by a host-key prompt. That leaves no state the
    // user cannot get out of: press Enter again and the question returns.
    if (!m_pending.isEmpty()) {
        emit confirmRequested(describePending(), m_pending);
        return;
    }
    const bool guard = m_settings == nullptr || m_settings->boolean("broadcast.confirmDangerous");
    // needsConfirm(), not just DangerousCommand. send() is Q_INVOKABLE, so this
    // is the trust boundary, not the strip's single-line text field: multi-line
    // text held back from ONE terminal by the paste guard must not fan out to
    // twelve unasked. describeRisk() supplies the wording for the other risks,
    // so the two guards cannot drift into describing the same text differently.
    const auto guarded = input::preparePaste(line, false);
    if (guard && guarded.needsConfirm()) {
        m_pending = line;
        m_pendingRisk = guarded.risk;
        emit confirmRequested(describePending(), line);
        return;
    }
    fanOut(line);
}

QString BroadcastModel::describePending() const {
    // The broadcast-specific sentence for the one risk that is about damage;
    // the paste guard's own wording for the rest, so "multiline" reads the same
    // here as it does on a paste.
    if (m_pendingRisk == input::PasteRisk::DangerousCommand) {
        return tr("This line can destroy data or escalate privileges, and broadcast will run it on "
                  "every selected session at once.");
    }
    return input::describeRisk(m_pendingRisk);
}

void BroadcastModel::resolve(bool allow) {
    const QString pending = std::exchange(m_pending, QString{});
    if (!allow || pending.isEmpty()) {
        // Nothing held. An empty line can never BE held — a bare Enter is not
        // destructive — so this is a second answer to a banner that has already
        // been answered, and sending an empty line for it would put a stray
        // Enter into every session.
        return;
    }
    if (m_state != BroadcastState::Active) {
        // The idle timer or a lost window can fire while the banner is up. Said
        // out loud rather than swallowed: the user pressed the button, and a
        // command that quietly went nowhere is worse than one that did not.
        emit reported(tr("Broadcast was paused while that was waiting, so nothing was sent."),
                      QString());
        return;
    }
    fanOut(pending);
}

void BroadcastModel::fanOut(const QString& line) {
    QStringList dropped;
    int sent = 0;
    for (Target& target : m_targets) {
        if (!target.marked) {
            continue;
        }
        bool accepted = false;
        if (target.tab.isNull()) {
            accepted = false;
        } else if (!QMetaObject::invokeMethod(target.tab, "sendBroadcast",
                                              Q_RETURN_ARG(bool, accepted), Q_ARG(QString, line))) {
            // A target with no such slot is a wiring bug, not a runtime state.
            // Treated as a refusal so it cannot look like a successful send.
            qWarning("broadcast: '%s' has no sendBroadcast(QString)", qPrintable(target.label));
            accepted = false;
        }
        if (accepted) {
            ++sent;
            continue;
        }
        // A session that cannot take input is NOT a target. Leaving it marked
        // would let the user go on believing twelve hosts are receiving when
        // nine are — the exact uncertainty the strip exists to remove.
        target.marked = false;
        dropped.append(target.label);
    }

    if (!dropped.isEmpty()) {
        rebuildRows();
        emit reported(tr("Dropped from the broadcast — these sessions are not connected, so they "
                         "received nothing."),
                      dropped.join(QStringLiteral(", ")));
    }
    if (markedCount() == 0) {
        stop();
        emit reported(tr("Broadcast stopped: nothing is left to send to."), QString());
        return;
    }
    restartIdleTimer();
    ++m_sentCount;
    emit delivered(sent);
}

void BroadcastModel::setState(BroadcastState state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    // A held line is deliberately NOT cleared here. Leaving Active while a
    // banner is up has to end in resolve() telling the user their button did
    // nothing — dropping the line here instead would make that answer
    // indistinguishable from a second click on an already-answered banner, and
    // both would then be silent. stop() clears it, which is the path that means
    // "this broadcast is over".
    restartIdleTimer();
    emit stateChanged();
}

void BroadcastModel::restartIdleTimer() {
    const int seconds = m_settings != nullptr
                            ? static_cast<int>(m_settings->integer("broadcast.idleSeconds"))
                            : kDefaultIdleSeconds;
    if (m_state != BroadcastState::Active || seconds <= 0) {
        m_idle->stop();
        return;
    }
    m_idle->start(seconds * 1000);
}

void BroadcastModel::rebuildRows() {
    // A target whose tab has gone never survives into the rows. QML destroys a
    // pane's delegate with deleteLater, so there is a window between the tab
    // being gone and forget() running in which the strip would otherwise still
    // NAME it — and a strip listing a session that no longer exists is exactly
    // the uncertainty this feature is supposed to remove.
    //
    // Safe from every caller: each one either finishes iterating m_targets
    // before calling this, or returns immediately afterwards.
    std::erase_if(m_targets, [](const Target& target) { return target.tab.isNull(); });
    m_rows.clear();
    m_rows.reserve(static_cast<qsizetype>(m_targets.size()));
    for (const Target& target : m_targets) {
        QVariantMap row;
        row[QStringLiteral("label")] = target.label;
        row[QStringLiteral("marked")] = target.marked;
        m_rows.append(row);
    }
    ++m_revision;
    emit tabsChanged();
}

}  // namespace krait::app
