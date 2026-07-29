import QtQuick
import Krait

Window {
    id: root
    width: 960
    height: 540
    visible: false  // shown from main() after the graphics configuration
    title: "Krait spike"
    color: "#0d0f17"

    SpikeGrid {
        anchors.fill: parent
    }
}
