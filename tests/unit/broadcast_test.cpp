// T74 — the broadcast interlock.
//
// WHAT THIS FILE COVERS. The strip is QML and is not tested here; what is
// tested is everything underneath it, which is where the interlock lives: who
// is a target, what the state machine allows, what a destructive-looking line
// does, and what happens when a target disconnects mid-broadcast.
//
// The fake tab below is the whole reason the model delivers through
// QMetaObject::invokeMethod rather than by including terminal_item.h. A real
// TerminalItem needs a GPU device, a window and a live pty; a QObject with a
// sendBroadcast(QString) slot needs none of those, and it is exactly the
// contract the real one honours.
//
// NOT covered here, and not mocked into looking covered: the strip itself, the
// keystrokes that reach it, and the QGuiApplication::applicationStateChanged
// connection that pauses a broadcast when the window loses focus. That
// connection is one line and there is no QGuiApplication in this binary; what
// IS tested is the transition it drives, by calling pause() directly.

#include "broadcast.h"
#include "input/paste.h"
#include <catch2/catch_test_macros.hpp>

#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <vector>

using krait::app::BroadcastModel;
using krait::app::BroadcastState;

namespace {

// A stand-in for one terminal. `connected` is what a real TerminalItem answers
// when it has a backend, a started session and a shell that has not exited.
class FakeTab : public QObject {
    Q_OBJECT

  public:
    bool connected = true;
    QStringList received;

    Q_INVOKABLE bool sendBroadcast(const QString& text) {
        if (!connected) {
            return false;
        }
        received.append(text);
        return true;
    }
};

int stateOf(const BroadcastModel& model) {
    return model.stateValue();
}

bool markedAt(const BroadcastModel& model, int index) {
    return model.tabs().at(index).toMap().value(QStringLiteral("marked")).toBool();
}

}  // namespace

TEST_CASE("broadcast: nothing is a target until it is offered", "[app][broadcast]") {
    BroadcastModel model;
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Off));
    CHECK(model.tabs().isEmpty());
    CHECK(model.markedCount() == 0);

    FakeTab one;
    model.offer(&one, QStringLiteral("web-01"));
    REQUIRE(model.tabs().size() == 1);
    // Offered is not selected. Registering a tab must never arm anything.
    CHECK(model.markedCount() == 0);
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Off));
}

TEST_CASE("broadcast: offering the same tab twice relabels rather than duplicates",
          "[app][broadcast]") {
    BroadcastModel model;
    FakeTab tab;
    model.offer(&tab, QStringLiteral("Shell"));
    model.offer(&tab, QStringLiteral("prod-db"));
    REQUIRE(model.tabs().size() == 1);
    CHECK(model.tabs().at(0).toMap().value(QStringLiteral("label")).toString() == "prod-db");
}

TEST_CASE("broadcast: begin marks the tab it was opened from and nothing else",
          "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    FakeTab two;
    model.offer(&one, QStringLiteral("web-01"));
    model.offer(&two, QStringLiteral("web-02"));

    model.begin(&two);
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Ready));
    CHECK(model.markedCount() == 1);
    CHECK(model.isMarked(&two));
    CHECK_FALSE(model.isMarked(&one));
}

TEST_CASE("broadcast: opening the strip is not arming it", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("web-01"));
    model.begin(&one);

    // Ready sends nothing. This is the whole reason Ready exists.
    model.send(QStringLiteral("uptime"));
    CHECK(one.received.isEmpty());

    model.arm();
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Active));
    model.send(QStringLiteral("uptime"));
    REQUIRE(one.received.size() == 1);
    CHECK(one.received.at(0) == "uptime");
}

TEST_CASE("broadcast: arming with nothing selected is refused and says so", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("web-01"));
    model.begin(nullptr);  // opened from a tab that is not a target

    const QSignalSpy reported(&model, &BroadcastModel::reported);
    model.arm();
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Ready));
    CHECK(reported.count() == 1);
}

TEST_CASE("broadcast: the line reaches every marked target and only those", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    FakeTab two;
    FakeTab three;
    model.offer(&one, QStringLiteral("web-01"));
    model.offer(&two, QStringLiteral("web-02"));
    model.offer(&three, QStringLiteral("db-01"));

    model.begin(&one);
    model.toggleAt(1);  // web-02 joins
    REQUIRE(model.markedCount() == 2);
    model.arm();

    const QSignalSpy delivered(&model, &BroadcastModel::delivered);
    model.send(QStringLiteral("systemctl restart nginx"));

    CHECK(one.received.size() == 1);
    CHECK(two.received.size() == 1);
    CHECK(three.received.isEmpty());
    REQUIRE(delivered.count() == 1);
    CHECK(delivered.at(0).at(0).toInt() == 2);
}

TEST_CASE("broadcast: unticking the last target disarms rather than arming at nobody",
          "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("web-01"));
    model.begin(&one);
    model.arm();
    REQUIRE(stateOf(model) == static_cast<int>(BroadcastState::Active));

    model.toggleAt(0);
    CHECK(model.markedCount() == 0);
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Ready));
}

// --- what happens when a target disconnects mid-broadcast -------------------

TEST_CASE("broadcast: a target that cannot take input is dropped, not swallowed",
          "[app][broadcast]") {
    BroadcastModel model;
    FakeTab live;
    FakeTab dead;
    model.offer(&live, QStringLiteral("web-01"));
    model.offer(&dead, QStringLiteral("web-02"));
    model.begin(&live);
    model.toggleAt(1);
    model.arm();

    // The shell on web-02 exits between one line and the next.
    model.send(QStringLiteral("uptime"));
    REQUIRE(dead.received.size() == 1);
    dead.connected = false;

    const QSignalSpy reported(&model, &BroadcastModel::reported);
    model.send(QStringLiteral("df -h"));

    // Delivered where it could, and the dead one is GONE from the set — a
    // target still ticked after it stopped receiving is the user believing two
    // hosts ran a command when one did.
    CHECK(live.received.size() == 2);
    CHECK(dead.received.size() == 1);
    CHECK_FALSE(model.isMarked(&dead));
    CHECK(model.markedCount() == 1);
    // And said out loud, naming the session.
    REQUIRE(reported.count() == 1);
    CHECK(reported.at(0).at(1).toString().contains("web-02"));
}

TEST_CASE("broadcast: the last target disconnecting ends the broadcast", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab only;
    model.offer(&only, QStringLiteral("web-01"));
    model.begin(&only);
    model.arm();
    only.connected = false;

    model.send(QStringLiteral("uptime"));
    CHECK(only.received.isEmpty());
    // Off, not Ready: there is nothing left to resume to.
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Off));
}

TEST_CASE("broadcast: a closed tab stops being a target", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    {
        FakeTab two;
        model.offer(&one, QStringLiteral("web-01"));
        model.offer(&two, QStringLiteral("web-02"));
        model.begin(&one);
        model.toggleAt(1);
        model.arm();
        REQUIRE(model.markedCount() == 2);
        model.forget(&two);
    }
    CHECK(model.tabs().size() == 1);
    CHECK(model.markedCount() == 1);

    model.send(QStringLiteral("uptime"));
    CHECK(one.received.size() == 1);
}

TEST_CASE("broadcast: a target destroyed WITHOUT forget() never receives", "[app][broadcast]") {
    // The lifetime claim the whole design rests on, and the one every other
    // test here sidesteps by calling forget() by hand. QML destroys a pane's
    // delegate with deleteLater, so there is a real window in which a marked
    // target is already gone and nothing has told the model yet.
    BroadcastModel model;
    FakeTab live;
    model.offer(&live, QStringLiteral("web-01"));
    {
        FakeTab doomed;
        model.offer(&doomed, QStringLiteral("web-02"));
        model.begin(&live);
        model.toggleAt(1);
        REQUIRE(model.markedCount() == 2);
        model.arm();
    }  // no forget(): the QPointer is now null and the model has not been told

    // The strip must not go on NAMING a session that no longer exists.
    model.send(QStringLiteral("uptime"));
    CHECK(live.received.size() == 1);
    CHECK(model.tabs().size() == 1);
    CHECK(model.markedCount() == 1);
    CHECK(model.tabs().at(0).toMap().value(QStringLiteral("label")).toString() == "web-01");
}

TEST_CASE("broadcast: closing every target ends the broadcast", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("web-01"));
    model.begin(&one);
    model.arm();

    const QSignalSpy reported(&model, &BroadcastModel::reported);
    model.forget(&one);
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Off));
    CHECK(reported.count() == 1);
}

// --- the destructive-command decision ---------------------------------------

TEST_CASE("broadcast: an ordinary line is never held for confirmation", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("web-01"));
    model.begin(&one);
    model.arm();

    const QSignalSpy confirm(&model, &BroadcastModel::confirmRequested);
    for (const char* line :
         {"uptime", "df -h", "systemctl status nginx", "cat /etc/hostname", "rm /tmp/one.log"}) {
        model.send(QString::fromLatin1(line));
    }
    // The whole point: the ordinary case costs nothing. A guard that fires on
    // every Enter is a guard people learn to press through.
    CHECK(confirm.count() == 0);
    CHECK(one.received.size() == 5);
}

TEST_CASE("broadcast: a destructive line is held until it is answered", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("prod-01"));
    model.begin(&one);
    model.arm();

    const QSignalSpy confirm(&model, &BroadcastModel::confirmRequested);
    model.send(QStringLiteral("rm -rf /var/lib/thing"));
    REQUIRE(confirm.count() == 1);
    // Nothing has gone anywhere yet.
    CHECK(one.received.isEmpty());
    // And the banner shows the line itself, not a paraphrase of it.
    CHECK(confirm.at(0).at(1).toString() == "rm -rf /var/lib/thing");

    model.resolve(true);
    REQUIRE(one.received.size() == 1);
    CHECK(one.received.at(0) == "rm -rf /var/lib/thing");
}

TEST_CASE("broadcast: a second line cannot replace the one the banner is showing",
          "[app][broadcast]") {
    // The banner names ONE command and the Accept button must run that one.
    // The strip is on every tab and stays armed while a line is held, so a
    // second Enter on another tab used to overwrite the held line — and
    // accepting the banner that said X then fanned out Y to every host.
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("prod-01"));
    model.begin(&one);
    model.arm();

    const QSignalSpy confirm(&model, &BroadcastModel::confirmRequested);
    model.send(QStringLiteral("sudo systemctl stop app"));
    REQUIRE(confirm.count() == 1);

    // A second dangerous line while the first is still unanswered.
    model.send(QStringLiteral("sudo rm -rf /srv"));
    // The question is re-asked about the ORIGINAL line, never replaced — and
    // re-asked rather than refused, so a banner lost to a host-key prompt can
    // always be brought back by pressing Enter again.
    REQUIRE(confirm.count() == 2);
    CHECK(confirm.at(1).at(1).toString() == "sudo systemctl stop app");

    model.resolve(true);
    REQUIRE(one.received.size() == 1);
    CHECK(one.received.at(0) == "sudo systemctl stop app");
}

TEST_CASE("broadcast: multi-line text is held too, not just dangerous commands",
          "[app][broadcast]") {
    // send() is Q_INVOKABLE, so the strip's single-line field is not the trust
    // boundary — this is. Text the paste guard holds back from ONE terminal
    // must not fan out to twelve unasked.
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("web-01"));
    model.begin(&one);
    model.arm();

    const QSignalSpy confirm(&model, &BroadcastModel::confirmRequested);
    model.send(QStringLiteral("cd /srv\nmake clean\nmake"));
    CHECK(confirm.count() == 1);
    CHECK(one.received.isEmpty());
    // And the wording is the paste guard's own, so the two cannot describe the
    // same text differently.
    CHECK(confirm.at(0).at(0).toString() ==
          krait::app::input::describeRisk(krait::app::input::PasteRisk::Multiline));
}

TEST_CASE("broadcast: refusing a held line sends nothing", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("prod-01"));
    model.begin(&one);
    model.arm();

    model.send(QStringLiteral("sudo reboot"));
    model.resolve(false);
    CHECK(one.received.isEmpty());
    // The held line is gone: a second "allow" cannot resurrect it.
    model.resolve(true);
    CHECK(one.received.isEmpty());
}

TEST_CASE("broadcast: a line held across a pause is not silently sent", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("prod-01"));
    model.begin(&one);
    model.arm();
    model.send(QStringLiteral("sudo rm -rf /srv"));

    // The idle timer or a lost window fires while the banner is up.
    model.pause();
    const QSignalSpy reported(&model, &BroadcastModel::reported);
    model.resolve(true);

    CHECK(one.received.isEmpty());
    // Not swallowed: the user pressed the button and has to be told why it did
    // nothing.
    CHECK(reported.count() == 1);
}

// --- the state machine's exits ----------------------------------------------

TEST_CASE("broadcast: pause keeps the selection, stop throws it away", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    FakeTab two;
    model.offer(&one, QStringLiteral("web-01"));
    model.offer(&two, QStringLiteral("web-02"));
    model.begin(&one);
    model.toggleAt(1);
    model.arm();

    // Pause is what focus loss and the idle timeout do. It drops to Ready with
    // the selection intact, because throwing away a twelve-host set every time
    // the user alt-tabs is what teaches people to leave the mode armed.
    model.pause();
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Ready));
    CHECK(model.markedCount() == 2);
    CHECK(markedAt(model, 0));
    CHECK(markedAt(model, 1));

    // Resuming needs no re-selection.
    model.arm();
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Active));

    model.stop();
    CHECK(stateOf(model) == static_cast<int>(BroadcastState::Off));
    CHECK(model.markedCount() == 0);
}

TEST_CASE("broadcast: pausing sends nothing until it is armed again", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    model.offer(&one, QStringLiteral("web-01"));
    model.begin(&one);
    model.arm();
    model.pause();

    model.send(QStringLiteral("uptime"));
    CHECK(one.received.isEmpty());
    model.arm();
    model.send(QStringLiteral("uptime"));
    CHECK(one.received.size() == 1);
}

TEST_CASE("broadcast: the row list is what the strip shows", "[app][broadcast]") {
    BroadcastModel model;
    FakeTab one;
    FakeTab two;
    model.offer(&one, QStringLiteral("web-01"));
    model.offer(&two, QStringLiteral("web-02"));
    model.begin(&two);

    REQUIRE(model.tabs().size() == 2);
    CHECK(model.tabs().at(0).toMap().value(QStringLiteral("label")).toString() == "web-01");
    CHECK_FALSE(markedAt(model, 0));
    CHECK(markedAt(model, 1));

    // The revision is what a QML binding hangs off, since isMarked() is a
    // method and registers no dependency of its own.
    const int before = model.revision();
    model.toggleAt(0);
    CHECK(model.revision() > before);
}

#include "broadcast_test.moc"
