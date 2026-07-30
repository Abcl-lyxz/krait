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
            onAccepted: {
                terminal.resolvePaste(true)
                banner.message = ""
                terminal.forceActiveFocus()
            }
            onRejected: {
                terminal.resolvePaste(false)
                banner.message = ""
                terminal.forceActiveFocus()
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
                banner.severity = "error"
                banner.showAccept = false
                banner.rejectText = qsTr("Dismiss")
                banner.detail = hint
                banner.message = message
                banner.forceActiveFocus()
            }

            onPasteConfirmRequested: (message, detail) => {
                banner.severity = "warning"
                banner.showAccept = true
                banner.acceptText = qsTr("Paste anyway")
                banner.rejectText = qsTr("Cancel")
                banner.detail = detail
                banner.message = message
                banner.forceActiveFocus()
            }
        }
    }

    Palette {
        id: palette
        z: 100

        onSessionChosen: (profileId) => {
            // The connection itself is not wired yet: opening a tab needs the
            // backend factory that T51 records as the next task. Saying so in
            // the banner beats a click that appears to do nothing.
            banner.severity = "info"
            banner.showAccept = false
            banner.rejectText = qsTr("Dismiss")
            banner.detail = ""
            banner.message = qsTr("Opening saved sessions is not wired up yet: %1").arg(profileId)
            banner.forceActiveFocus()
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
