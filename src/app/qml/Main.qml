import QtQuick
import Krait

Window {
    id: root
    width: 960
    height: 540
    visible: false  // shown from main() after the graphics configuration
    title: qsTr("Krait")
    color: "#0d0f17"

    // Ctrl+Shift+P from anywhere in the window. rules/ui.md is explicit that a
    // feature reachable only by mouse is incomplete work, and the palette is
    // the thing that makes every other action reachable at all.
    Shortcut {
        sequence: "Ctrl+Shift+P"
        onActivated: palette.open()
    }
    Shortcut {
        sequence: "Ctrl+Shift+O"
        onActivated: palette.open()
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: settingsPage.open()
    }

    // Bench runs keep the synthetic spike; normal runs are the terminal.
    SpikeGrid {
        anchors.fill: parent
        visible: benchMode
    }

    // The banner sits ABOVE the terminal and shrinks it, rather than floating
    // over it: rules/ui.md bans anything modal, and a strip that covers output
    // is the same mistake in a different shape.
    Column {
        anchors.fill: parent
        visible: !benchMode

        Banner {
            id: banner
            width: parent.width

            // What the banner is currently asking about. One banner serves
            // paste confirmation, host-key trust and credentials, so the answer
            // has to be routed to whoever asked — the alternative is three
            // stacked strips, and a tab is only so tall.
            property string mode: "paste"

            // Leaving the credential mode wipes the field, whatever the reason
            // for leaving. Without this the ONLY path that clears a typed
            // password is the user pressing Dismiss: a connection that fails
            // while the prompt is open replaces the message and leaves the
            // plaintext sitting in the TextInput (rules/net.md — never in
            // memory longer than needed).
            onModeChanged: if (mode !== "credential") banner.endInput()

            function dismiss() {
                banner.endInput()
                banner.message = ""
                banner.mode = "paste"
                terminal.forceActiveFocus()
            }

            onAccepted: {
                if (banner.mode === "credential") {
                    terminal.respondCredential(banner.inputText, banner.remember)
                } else if (banner.mode === "hostkey") {
                    terminal.respondHostKey(true)
                } else if (banner.mode === "paste") {
                    terminal.resolvePaste(true)
                }
                banner.dismiss()
            }
            onRejected: {
                if (banner.mode === "credential") {
                    // An empty answer is how the backend hears "cancelled".
                    terminal.respondCredential("", false)
                } else if (banner.mode === "hostkey") {
                    terminal.respondHostKey(false)
                } else if (banner.mode === "paste") {
                    terminal.resolvePaste(false)
                }
                banner.dismiss()
            }
        }

        TerminalView {
            id: terminal
            width: parent.width
            height: parent.height - banner.height
            focus: !benchMode && !banner.visible

            // T33: a backend failure is a per-tab banner with no accept action —
            // there is nothing to allow, only something to acknowledge.
            onErrorRaised: (message, hint) => {
                // A changed host key arrives as TWO signals: the prompt with the
                // fingerprint and randomart, then the taxonomy error, both
                // queued in the same turn. Letting the second overwrite the
                // first meant the danger banner existed for less than a frame
                // and the user never saw the evidence — which is the entire
                // point of the changed-key screen (rules/net.md).
                if (banner.mode === "hostkey") {
                    return
                }
                banner.mode = "error"
                banner.severity = "error"
                banner.showAccept = false
                banner.rejectText = qsTr("Dismiss")
                banner.detail = hint
                banner.message = message
                banner.forceActiveFocus()
            }

            onPasteConfirmRequested: (message, detail) => {
                banner.mode = "paste"
                banner.severity = "warning"
                banner.showAccept = true
                banner.acceptText = qsTr("Paste anyway")
                banner.rejectText = qsTr("Cancel")
                banner.detail = detail
                banner.message = message
                banner.forceActiveFocus()
            }

            // T52. `askable` false is a changed or unverifiable key: there is
            // no Trust button at all, because rules/net.md says that is never
            // a question, and a button that refuses is worse than no button.
            onHostKeyPromptRequested: (message, detail, askable) => {
                banner.mode = "hostkey"
                banner.severity = askable ? "warning" : "danger"
                banner.showAccept = askable
                banner.acceptText = qsTr("Trust this server")
                banner.rejectText = askable ? qsTr("Do not connect") : qsTr("Dismiss")
                banner.detail = detail
                banner.message = message
                banner.forceActiveFocus()
            }

            onCredentialPromptRequested: (prompt, echo) => {
                banner.mode = "credential"
                banner.severity = "warning"
                banner.showAccept = true
                banner.acceptText = qsTr("Connect")
                banner.rejectText = qsTr("Cancel")
                banner.detail = ""
                // The prompt is SERVER-CONTROLLED, and Banner is visible only
                // while message is non-empty — so a server that sends an empty
                // keyboard-interactive name, instruction and prompt would
                // produce an invisible banner while the worker sits in its
                // five-minute answer wait. The connection would look hung with
                // nothing on screen to explain it.
                banner.message = prompt.length > 0
                                 ? prompt
                                 : qsTr("The server is asking for a password.")
                // Only offer to remember what is actually a stored secret. A
                // keyboard-interactive challenge is frequently a one-time code,
                // and saving one of those is saving nothing.
                banner.beginInput(echo, !echo)
            }

            // Progress, not a decision: no buttons, and an empty message means
            // whatever it was reporting is over.
            onConnectionNotice: (message) => {
                if (message.length === 0) {
                    if (banner.mode === "notice") {
                        banner.dismiss()
                    }
                    return
                }
                banner.mode = "notice"
                banner.severity = "warning"
                banner.showAccept = false
                banner.rejectText = qsTr("Dismiss")
                banner.detail = ""
                banner.message = message
            }
        }
    }

    Palette {
        id: palette
        z: 100

        // T52: the session itself is opened in C++ (main() connects
        // SessionModel::sessionRequested), because looking a profile up and
        // building a backend is a decision, and rules/ui.md keeps those out of
        // views. All that is left here is where the keyboard goes.
        onSessionChosen: (profileId) => {
            banner.dismiss()
        }

        onActionChosen: (actionId) => {
            if (actionId === "settings.open") {
                settingsPage.open()
                return
            }
            banner.severity = "info"
            banner.showAccept = false
            banner.rejectText = qsTr("Dismiss")
            banner.detail = ""
            banner.message = qsTr("Not wired up yet: %1").arg(actionId)
            banner.forceActiveFocus()
        }

        onDismissed: terminal.forceActiveFocus()
    }

    Settings {
        id: settingsPage
        z: 90
        onDismissed: terminal.forceActiveFocus()
    }
}
