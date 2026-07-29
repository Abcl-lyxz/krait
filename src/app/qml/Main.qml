import QtQuick
import Krait

Window {
    id: root
    width: 960
    height: 540
    visible: false  // shown from main() after the graphics configuration
    title: "Krait spike"
    color: "#0d0f17"

    // Bench runs keep the synthetic spike; normal runs are the terminal.
    SpikeGrid {
        anchors.fill: parent
        visible: benchMode
    }
    TerminalView {
        anchors.fill: parent
        visible: !benchMode
        focus: !benchMode
    }
}
