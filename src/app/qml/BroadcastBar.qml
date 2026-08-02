import QtQuick
import Krait

// Broadcast, with its interlock on screen (plan T74).
//
// This strip IS the interlock's visible half. rules/ui.md bans a menu bar and
// app-modal surfaces, so the thing that answers "which sessions are my
// keystrokes reaching" has to be a persistent strip like the tunnel list and
// the snippet bar — and it lives on EVERY tab, because a tab you can switch to
// that does not name the targets is exactly the uncertainty this prevents.
//
// The input line is deliberate too. While broadcast is armed the line is
// composed HERE, not in the terminal, and only Enter sends it. That is what
// makes the payload and the target set visible at the same moment, and it is
// what gives the destructive-command check something discrete to look at —
// there is no unit to classify in a stream of live keystrokes.
Rectangle {
    id: bar

    // The shared BroadcastModel, owned by Main.qml. Null in a pane built
    // before the model exists, so every use is guarded.
    property var broadcast: null

    signal closeRequested

    // BroadcastState: 0 Off, 1 Ready, 2 Active. NOT called `state`: Item already has
    // one, of a different type, and shadowing it is a defect the QML compiler
    // only reports as unreachable code in a generated file.
    readonly property int phase: bar.broadcast ? bar.broadcast.state : 0
    readonly property bool armed: bar.phase === 2

    // Cleared only once the line has ACTUALLY gone out, so a line held for
    // confirmation stays readable and editable while the banner is up.
    //
    // A bound counter rather than Connections { target: bar.broadcast }:
    // `target` is a QObject* and `broadcast` is a var, and the conversion the
    // QML compiler generates for that pair trips /W4 /WX inside Qt's own
    // headers. Reading through the var chain stays interpreted, which is what
    // every other line in this file already does.
    readonly property int sentCount: bar.broadcast ? bar.broadcast.sentCount : 0
    onSentCountChanged: input.text = ""

    // TODO(theme): tokens once the theme system exists (M5). Matched to the
    // snippet and tunnel strips so they read as one application — except while
    // armed, where the strip is deliberately the loudest thing on screen.
    color: bar.armed ? Theme.wash(Theme.danger, 0.88) : Theme.surface
    height: visible ? content.implicitHeight + 12 : 0

    function toggleArmed() {
        if (!bar.broadcast) {
            return
        }
        if (bar.armed) {
            bar.broadcast.pause()
        } else {
            bar.broadcast.arm()
        }
    }

    function focusInput() {
        input.forceActiveFocus()
    }

    Column {
        id: content
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        Row {
            width: parent.width
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: bar.armed ? qsTr("BROADCAST ON") : qsTr("BROADCAST READY")
                color: bar.armed ? Theme.danger : Theme.warning
                font.bold: true
                textFormat: Text.PlainText
            }

            // The target set, named and always on screen. The number is the
            // toggle shortcut, the way SnippetBar's numbers are — and it is
            // also what tells two sessions called "Shell" apart.
            Flow {
                width: parent.width - 260
                spacing: 6

                Repeater {
                    model: bar.broadcast ? bar.broadcast.tabs : []

                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        required property int index

                        color: chip.modelData.marked ? Theme.wash(Theme.danger, 0.6) : Theme.surfaceAlt
                        radius: 3
                        width: chipLabel.implicitWidth + 16
                        height: chipLabel.implicitHeight + 6

                        Text {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: chip.index < 9
                                  ? qsTr("%1. %2").arg(chip.index + 1).arg(chip.modelData.label)
                                  : chip.modelData.label
                            color: chip.modelData.marked ? Qt.lighter(Theme.danger, 1.5) : Theme.textDim
                            textFormat: Text.PlainText
                        }

                        TapHandler {
                            // Only while it is safe to change the set. Retargeting
                            // a live broadcast with a click is how the wrong host
                            // joins one.
                            enabled: !bar.armed
                            onTapped: {
                                if (bar.broadcast) {
                                    bar.broadcast.toggleAt(chip.index)
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            width: parent.width
            height: inputRow.implicitHeight + 8
            color: Theme.bg
            radius: 3

            Row {
                id: inputRow
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 6

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: bar.armed ? "»" : "·"
                    color: bar.armed ? Theme.danger : Theme.textFaint
                    font.family: "Cascadia Mono"
                }

                TextInput {
                    id: input
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 30
                    color: Theme.text
                    font.family: "Cascadia Mono"
                    selectByMouse: true
                    // Nothing can be typed until the broadcast is armed. The
                    // field being dead is the honest reading of Ready: the
                    // targets are chosen, and nothing is going anywhere.
                    readOnly: !bar.armed
                    focus: bar.visible

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: input.text.length === 0
                        color: Theme.textFaint
                        textFormat: Text.PlainText
                        text: bar.armed
                              ? qsTr("Enter sends this line to every session above.")
                              : qsTr("Pick sessions with 1-9 or the mouse, then Ctrl+Enter to start.")
                    }

                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Escape) {
                            if (bar.broadcast) {
                                bar.broadcast.stop()
                            }
                            bar.closeRequested()
                            event.accepted = true
                            return
                        }
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && (event.modifiers & Qt.ControlModifier)) {
                            bar.toggleArmed()
                            event.accepted = true
                            return
                        }
                        // Digits pick targets only while nothing is armed —
                        // once it is, a digit is part of the command being
                        // typed and must not silently retarget the broadcast.
                        if (!bar.armed && event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
                            if (bar.broadcast) {
                                bar.broadcast.toggleAt(event.key - Qt.Key_1)
                            }
                            event.accepted = true
                        }
                    }

                    onAccepted: {
                        if (bar.broadcast && bar.armed) {
                            bar.broadcast.send(input.text)
                        }
                    }
                }
            }
        }
    }
}
