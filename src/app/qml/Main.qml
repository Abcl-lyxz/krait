import QtQuick
import Krait

Window {
    id: root
    width: 960
    height: 540
    visible: false  // shown from main() after the graphics configuration
    title: "Krait"
    color: "#0d0f17"

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
}
