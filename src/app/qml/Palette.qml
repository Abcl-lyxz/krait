import QtQuick
import Krait

// The command palette: one list holding both commands and saved sessions,
// because "what I want to do" and "where I want to be" are the same question to
// the person typing.
//
// rules/ui.md: views only. Every decision — what matches, how it ranks, what an
// entry means when activated — is in SessionModel and the pure functions behind
// it. This file positions rectangles and forwards keys.
//
// It is an overlay rather than a strip, which is the one place this differs from
// Banner: a palette is modal to the KEYBOARD by nature (it exists to capture
// what you type) but never to the application — Escape always closes it, output
// underneath keeps arriving, and nothing waits on it.
Item {
    id: palette

    signal actionChosen(string actionId)
    signal sessionChosen(string profileId)
    signal dismissed

    visible: false
    anchors.fill: parent

    function open() {
        sessions.query = ""
        field.text = ""
        list.currentIndex = 0
        palette.visible = true
        field.forceActiveFocus()
    }

    function close() {
        palette.visible = false
        palette.dismissed()
    }

    SessionModel {
        id: sessions
        onActionRequested: (actionId) => {
            palette.close()
            palette.actionChosen(actionId)
        }
        onSessionRequested: (profileId) => {
            palette.close()
            palette.sessionChosen(profileId)
        }
    }

    // Catches clicks outside the panel so they dismiss rather than falling
    // through to the terminal, which would move the cursor behind an overlay the
    // user is still looking at.
    MouseArea {
        anchors.fill: parent
        onClicked: palette.close()
    }

    Rectangle {
        color: "#000000"
        opacity: 0.45
        anchors.fill: parent
    }

    Rectangle {
        id: panel
        width: Math.min(parent.width - 80, 640)
        height: Math.min(parent.height - 80, 420)
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(parent.height * 0.12)
        color: "#161923"
        border.color: "#2c3242"
        border.width: 1
        radius: 6

        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            TextInput {
                id: field
                width: parent.width
                color: "#e6e9f0"
                font.pixelSize: 16
                selectByMouse: true
                onTextChanged: {
                    sessions.query = text
                    list.currentIndex = 0
                }

                Text {
                    anchors.fill: parent
                    visible: field.text.length === 0
                    color: "#5b6478"
                    font: field.font
                    text: qsTr("Type a command or a session…")
                }

                // Arrows move the selection, Enter takes it, Escape leaves. A
                // palette you have to reach for the mouse in is a palette that
                // has already failed (rules/ui.md: keyboard-first).
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                Keys.onEscapePressed: palette.close()
                Keys.onReturnPressed: sessions.activate(list.currentIndex)
                Keys.onEnterPressed: sessions.activate(list.currentIndex)
            }

            Rectangle {
                width: parent.width
                height: 1
                color: "#2c3242"
            }

            Text {
                width: parent.width
                visible: sessions.count === 0
                color: "#5b6478"
                text: qsTr("Nothing matches.")
            }

            ListView {
                id: list
                width: parent.width
                height: parent.height - field.height - 32
                clip: true
                model: sessions.entries
                currentIndex: 0
                highlightMoveDuration: 0

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property var modelData

                    width: list.width
                    height: 28
                    color: row.index === list.currentIndex ? "#243049" : "transparent"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 8
                        width: parent.width - detail.width - 24
                        color: row.modelData.kind === "session" ? "#9ecbff" : "#e6e9f0"
                        text: row.modelData.label
                        elide: Text.ElideRight
                    }

                    Text {
                        id: detail
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        color: "#5b6478"
                        text: row.modelData.detail
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            list.currentIndex = row.index
                            sessions.activate(row.index)
                        }
                    }
                }
            }
        }
    }
}
