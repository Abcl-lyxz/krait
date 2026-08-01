import QtQuick

// The tab bar (plan T53). Hand-rolled for the same reason BannerButton is:
// this needs a row of labels and a close box, and QtQuick.Controls would pull a
// whole styling stack in for them.
//
// rules/ui.md: keyboard-first. Everything here is reachable without the mouse —
// Ctrl+Shift+T/W, Ctrl+Tab, Ctrl+1..9 — and the strip is the visible half of
// those actions, not the only way to reach them.
Item {
    id: strip

    // Rows of { title, accent }. Owned by Main.qml, which owns the tabs.
    property var tabs: null
    property int currentIndex: 0

    signal selected(int index)
    signal closed(int index)
    signal newTab

    readonly property int tabCount: strip.tabs ? strip.tabs.count : 0

    implicitHeight: 30
    // One tab is not a tab bar, it is a title. Hidden until there are two,
    // so the common case gives its rows to the terminal.
    visible: strip.tabCount > 1
    height: visible ? implicitHeight : 0

    // TODO(theme): tokens once the theme system exists (same debt as Banner).
    readonly property color barBg: "#12141c"
    readonly property color tabBg: "#1b1f2b"
    readonly property color tabFg: "#e6e9f0"
    readonly property color idleFg: "#7c869e"

    Rectangle {
        anchors.fill: parent
        color: strip.barBg
    }

    Row {
        id: row
        anchors.fill: parent
        spacing: 1

        Repeater {
            model: strip.tabs

            delegate: Rectangle {
                id: item
                required property int index
                required property string title
                required property string accent
                required property bool broadcasting

                readonly property bool current: item.index === strip.currentIndex

                width: Math.min(200, Math.max(90, strip.width / Math.max(1, strip.tabCount)))
                height: strip.height
                color: item.current ? strip.tabBg : "transparent"

                // The safety accent (rules/ui.md: prod = red is a core UX
                // invariant, never behind a toggle). A stripe rather than a
                // tinted background so it reads the same on the current tab and
                // the ones behind it.
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 2
                    visible: item.accent.length > 0
                    color: item.accent.length > 0 ? item.accent : "transparent"
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    onClicked: (mouse) => {
                        // Middle-click closes, which is the habit from every
                        // browser and every other terminal.
                        if (mouse.button === Qt.MiddleButton) {
                            strip.closed(item.index)
                        } else {
                            strip.selected(item.index)
                        }
                    }
                }

                // T74. This tab is a broadcast target. On the tab itself and
                // not only in the broadcast strip, so the answer to "is this
                // one of them" survives the tab being behind another.
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 3
                    visible: item.broadcasting
                    text: "»"
                    color: "#f38ba8"
                    font.bold: true
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: item.broadcasting ? 16 : 10
                    width: parent.width - close.width - (item.broadcasting ? 24 : 18)
                    text: item.title
                    color: item.current ? strip.tabFg : strip.idleFg
                    elide: Text.ElideMiddle
                    // A session name can come from an imported PuTTY registry
                    // or a hand-edited TOML, so it is not ours to trust as
                    // markup.
                    textFormat: Text.PlainText
                }

                // After the full-tab MouseArea in declaration order, so its own
                // area wins the click rather than selecting the tab.
                Text {
                    id: close
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    text: "×"
                    color: closeArea.containsMouse ? strip.tabFg : strip.idleFg
                    font.pixelSize: 16

                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        onClicked: strip.closed(item.index)
                    }
                }
            }
        }

        Rectangle {
            width: 28
            height: strip.height
            color: plusArea.containsMouse ? strip.tabBg : "transparent"

            Text {
                anchors.centerIn: parent
                text: "+"
                color: plusArea.containsMouse ? strip.tabFg : strip.idleFg
                font.pixelSize: 16
            }

            MouseArea {
                id: plusArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: strip.newTab()
            }
        }
    }
}
