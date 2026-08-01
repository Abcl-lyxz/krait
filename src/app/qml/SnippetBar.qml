import QtQuick

// The snippet bar (plan T69): the profile's named snippets, one click or one
// keystroke from the session.
//
// A strip, like the tunnel list and the file panel, for the reason rules/ui.md
// gives: a surface you have to dismiss before you can type is one nobody leaves
// open, and an app-modal one is a bug. It sits above the terminal and takes the
// height it needs.
Rectangle {
    id: bar

    // The TerminalView this belongs to. Null between panes closing and the next
    // binding evaluating, so every use is guarded.
    property Item terminal: null

    signal closeRequested

    readonly property var rows: terminal ? terminal.snippets : []
    // What the pointer is over, shown below — a snippet named "restart" that is
    // actually something else is the whole hazard here, and the preview is the
    // only thing that closes it.
    property string preview: ""

    // 1-9 send the first nine directly. rules/ui.md: a feature reachable only by
    // mouse is incomplete work, and a bar you have to arrow through to reach the
    // third item is one people go back to typing instead.
    function sendAt(index) {
        if (bar.terminal && index >= 0 && index < bar.rows.length) {
            bar.terminal.sendSnippet(index)
            bar.closeRequested()
        }
    }

    // TODO(theme): tokens once the theme system exists (M5). Matched to the
    // tunnel strip so the two do not look like different applications.
    color: "#12141c"
    height: visible ? content.implicitHeight + 12 : 0
    focus: visible

    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Escape) {
            bar.closeRequested()
            event.accepted = true
            return
        }
        if (event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            bar.sendAt(event.key - Qt.Key_1)
            event.accepted = true
        }
    }

    Column {
        id: content
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        Text {
            visible: bar.rows.length === 0
            width: parent.width
            text: qsTr("This session has no snippets. Add a snippets = list to it in sessions.toml.")
            color: "#7c869e"
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }

        Flow {
            width: parent.width
            spacing: 6

            Repeater {
                model: bar.rows

                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    required property int index

                    color: hover.hovered ? "#2c3242" : "#1c2030"
                    radius: 3
                    width: label.implicitWidth + 16
                    height: label.implicitHeight + 8

                    Text {
                        id: label
                        anchors.centerIn: parent
                        // The number is the shortcut, so the bar teaches its own
                        // keyboard path instead of needing to be documented.
                        text: chip.index < 9
                              ? qsTr("%1. %2").arg(chip.index + 1).arg(chip.modelData.name)
                              : chip.modelData.name
                        color: "#e6e9f0"
                        textFormat: Text.PlainText
                    }

                    HoverHandler {
                        id: hover
                        onHoveredChanged: bar.preview = hovered ? chip.modelData.preview : ""
                    }

                    TapHandler {
                        onTapped: bar.sendAt(chip.index)
                    }
                }
            }
        }

        Text {
            visible: bar.preview.length > 0
            width: parent.width
            text: bar.preview
            color: "#7c869e"
            font.family: "Cascadia Mono"
            textFormat: Text.PlainText
            elide: Text.ElideRight
        }
    }
}
