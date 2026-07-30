import QtQuick

// A flat button for Banner.qml. Hand-rolled rather than QtQuick.Controls: the
// banner needs two buttons and nothing else, and Controls would pull a whole
// styling stack in for them.
Rectangle {
    id: button

    property alias text: label.text
    property color accent: "#cdd6f4"

    signal clicked

    implicitWidth: label.implicitWidth + 22
    implicitHeight: label.implicitHeight + 12
    radius: 4
    color: mouse.pressed
           ? Qt.rgba(button.accent.r, button.accent.g, button.accent.b, 0.28)
           : mouse.containsMouse
             ? Qt.rgba(button.accent.r, button.accent.g, button.accent.b, 0.16)
             : "transparent"
    border.width: 1
    border.color: Qt.rgba(button.accent.r, button.accent.g, button.accent.b, 0.5)

    Text {
        id: label
        anchors.centerIn: parent
        color: button.accent
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
