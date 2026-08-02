import QtQuick
import Krait

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

    // "warning" (a risky paste, an unknown host key), "error" (a backend
    // failure, T33) or "danger" (T52: a changed host key — the one case where
    // the right answer is to stop, so it does not share the error palette).
    property string severity: "warning"
    property string message: ""

    // T52: a credential prompt. The banner grows a field rather than opening a
    // dialog, because rules/ui.md bans app-modal surfaces in session flows and
    // a password box is exactly where every other terminal reaches for one.
    property bool showInput: false
    property bool inputEcho: true
    // Whether to offer storing it in the vault, and the current answer.
    property bool showRemember: false
    property bool remember: false
    property alias inputText: input.text

    function beginInput(echo, offerRemember) {
        input.text = ""
        banner.inputEcho = echo
        banner.showRemember = offerRemember
        banner.remember = false
        banner.showInput = true
        input.forceActiveFocus()
    }

    function endInput() {
        // Overwritten, not just hidden: a QString holding a password that stays
        // reachable from QML is one more copy than rules/net.md wants. The
        // copies TextInput already made are the ceiling here (see the note on
        // SshBackend::respondCredential).
        input.text = ""
        banner.showInput = false
        banner.showRemember = false
    }
    // Optional second line: the detail a user needs in order to decide, e.g.
    // the first line of what is about to be pasted.
    property string detail: ""
    property string acceptText: qsTr("Allow (Ctrl+Enter)")
    property string rejectText: qsTr("Cancel")
    property bool showAccept: true

    signal accepted
    signal rejected

    // TODO(T31): these come from the theme system once it exists. rules/ui.md
    // makes hex literals in QML a defect, and this is the debt that buys the
    // banner before the theme lands.
    readonly property color warningBg: Theme.wash(Theme.warning, 0.82)
    readonly property color warningFg: Theme.warning
    readonly property color errorBg: Theme.wash(Theme.danger, 0.82)
    readonly property color errorFg: Theme.danger
    // Louder than error on purpose: a changed host key is the only banner in
    // the app that says "stop", and it must not look like the one that says
    // "the shell exited".
    readonly property color dangerBg: Theme.wash(Theme.danger, 0.72)
    readonly property color dangerFg: Qt.lighter(Theme.danger, 1.3)
    readonly property color foreground: severity === "danger"
                                        ? dangerFg
                                        : severity === "error" ? errorFg : warningFg

    color: severity === "danger" ? dangerBg : severity === "error" ? errorBg : warningBg
    height: visible ? layout.implicitHeight + 20 : 0
    visible: message.length > 0

    // Keyboard-first (rules/ui.md), but accept is CTRL+Enter and never plain
    // Enter. The banner takes focus the instant Ctrl+Shift+V is pressed, and
    // Enter is the reflex keystroke right after pasting a command — plain
    // Enter here would confirm a `sudo` paste nobody read, which is the exact
    // outcome the paste guard exists to prevent.
    focus: visible
    Keys.onEscapePressed: banner.rejected()
    Keys.onReturnPressed: (event) => banner.handleConfirmKey(event)
    Keys.onEnterPressed: (event) => banner.handleConfirmKey(event)

    function handleConfirmKey(event) {
        // The Ctrl requirement exists for the PASTE guard, where Enter is the
        // reflex keystroke right after pasting and would confirm a `sudo` line
        // nobody read. A credential field is the opposite case: Enter is what
        // everyone presses after typing a password, and demanding Ctrl there
        // would read as the field being broken.
        if (showAccept && (showInput || (event.modifiers & Qt.ControlModifier))) {
            accepted()
        } else {
            event.accepted = true  // swallow it: never fall through to the terminal
        }
    }

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
                // Bounded, like `detail` below. A keyboard-interactive prompt
                // is server-controlled and the sanitiser allows twelve lines
                // per field across three fields — thirty-six lines of banner
                // would drive TerminalView's height negative and stop the grid
                // updating at all.
                maximumLineCount: 6
                elide: Text.ElideRight
                // PlainText, not the AutoText default: `detail` below carries
                // raw clipboard text, and AutoText would let a pasted <b> or
                // <img src="http://..."> restyle the very warning that is about
                // it, or fetch a remote resource. The message gets the same
                // treatment so the two cannot drift.
                textFormat: Text.PlainText
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
                textFormat: Text.PlainText  // see above: this is hostile input
                font.family: "Cascadia Mono"
            }

            // T52: the credential field. TextInput and not QtQuick.Controls,
            // for the same reason BannerButton is hand-rolled — this needs one
            // field, and Controls would pull a styling stack in for it.
            Rectangle {
                width: parent.width
                height: banner.showInput ? input.implicitHeight + 10 : 0
                visible: banner.showInput
                color: Qt.rgba(0, 0, 0, 0.35)
                radius: 3
                border.width: 1
                border.color: Qt.rgba(banner.foreground.r, banner.foreground.g,
                                      banner.foreground.b, input.activeFocus ? 0.8 : 0.35)

                TextInput {
                    id: input
                    anchors.fill: parent
                    anchors.margins: 5
                    color: banner.foreground
                    font.family: "Cascadia Mono"
                    selectByMouse: true
                    // Password by default. echoMode is the only thing standing
                    // between a shoulder and a passphrase, so it is driven by
                    // the backend's `echo` flag rather than guessed from the
                    // prompt text, which is server-controlled.
                    echoMode: banner.inputEcho ? TextInput.Normal : TextInput.Password

                    Keys.onEscapePressed: banner.rejected()
                    Keys.onReturnPressed: banner.accepted()
                    Keys.onEnterPressed: banner.accepted()
                    // Keyboard-first (rules/ui.md): the remember toggle is
                    // reachable without the mouse, and the label below says so
                    // rather than leaving it to be discovered.
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_R && (event.modifiers & Qt.ControlModifier)) {
                            banner.remember = !banner.remember
                            event.accepted = true
                        }
                    }
                }
            }

            Text {
                width: parent.width
                visible: banner.showRemember
                color: banner.foreground
                opacity: 0.75
                textFormat: Text.PlainText
                text: banner.remember
                      ? qsTr("Will be saved to the Windows vault — Ctrl+R to stop")
                      : qsTr("Not saved — Ctrl+R to remember it")

                MouseArea {
                    anchors.fill: parent
                    onClicked: banner.remember = !banner.remember
                }
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
