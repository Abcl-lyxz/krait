import QtQuick

// The per-tab banner (plan T28). rules/ui.md: "Errors are per-tab banners with
// error-taxonomy codes from the backend layer. QMessageBox and any app-modal
// surface are banned in session flows." So this is a strip inside the tab, not
// a dialog: it never blocks the rest of the app and never steals a window.
//
// T28 uses it to confirm a risky paste; T33 wires backend errors into the same
// component, which is why severity and the actions are parameters rather than
// baked in.
Rectangle {
    id: banner

    // "warning" (a risky paste) or "error" (a backend failure, T33).
    property string severity: "warning"
    property string message: ""
    // Optional second line: the detail a user needs in order to decide, e.g.
    // the first line of what is about to be pasted.
    property string detail: ""
    property string acceptText: qsTr("Allow")
    property string rejectText: qsTr("Cancel")
    property bool showAccept: true

    signal accepted
    signal rejected

    // TODO(T31): these come from the theme system once it exists. rules/ui.md
    // makes hex literals in QML a defect, and this is the debt that buys the
    // banner before the theme lands.
    readonly property color warningBg: "#3a2f1a"
    readonly property color warningFg: "#f9e2af"
    readonly property color errorBg: "#3a1f26"
    readonly property color errorFg: "#f38ba8"
    readonly property color foreground: severity === "error" ? errorFg : warningFg

    color: severity === "error" ? errorBg : warningBg
    height: visible ? layout.implicitHeight + 20 : 0
    visible: message.length > 0

    // Keyboard-first (rules/ui.md): Esc rejects, Enter accepts. A banner
    // reachable only by mouse is incomplete work.
    focus: visible
    Keys.onEscapePressed: banner.rejected()
    Keys.onReturnPressed: if (banner.showAccept) banner.accepted()
    Keys.onEnterPressed: if (banner.showAccept) banner.accepted()

    Row {
        id: layout
        anchors.fill: parent
        anchors.margins: 10
        spacing: 12

        Column {
            width: parent.width - actions.width - parent.spacing
            spacing: 4

            Text {
                width: parent.width
                text: banner.message
                color: banner.foreground
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: banner.detail
                visible: banner.detail.length > 0
                color: banner.foreground
                opacity: 0.75
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.WrapAnywhere
                font.family: "Cascadia Mono"
            }
        }

        Row {
            id: actions
            spacing: 8
            anchors.verticalCenter: parent.verticalCenter

            BannerButton {
                text: banner.acceptText
                visible: banner.showAccept
                accent: banner.foreground
                onClicked: banner.accepted()
            }
            BannerButton {
                text: banner.rejectText
                accent: banner.foreground
                onClicked: banner.rejected()
            }
        }
    }
}
