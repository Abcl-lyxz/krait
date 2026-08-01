import QtQuick

// The SFTP dual-pane file panel (plan T65).
//
// A strip inside the tab, not a window: rules/ui.md bans app-modal surfaces in
// session flows, and a transfer you have to dismiss to type is one nobody
// leaves open while a build runs.
//
// T73 added two things to it: opening a remote file in the user's editor and
// watching it (the yellow strip above the footer), and installing Krait's shell
// integration onto the server (the sheet that covers the panel).
//
// Every decision lives in SftpModel (src/app/sftp_model.h) — where a path
// composes to, whether a name may be used, which reply belongs to which pane,
// and what a confirmation is allowed to say. This file repeats what the model
// says and reports what was clicked.
//
// TODO(theme): the literals below become tokens once the theme system exists,
// the same way SessionPane.qml's dividers do.
Rectangle {
    id: panel

    // The focused terminal's SftpModel. Null before a terminal exists.
    property var files: null

    signal closeRequested

    color: "#12141c"
    clip: true

    // The MouseArea whose row is being dragged, or null.
    //
    // A MouseArea rather than a bool because Drag.active has to follow the REAL
    // drag state: MouseArea reports it only once the drag threshold is passed,
    // so a press that never moves does not send a drag-enter to the other pane
    // and a click cannot be mistaken for a transfer.
    property var dragSource: null

    // One proxy for both panes rather than dragging the delegates themselves:
    // a ListView positions its delegates, so moving one leaves a hole in the
    // list and it snaps back when the model refreshes.
    Item {
        id: dragProxy
        width: 1
        height: 1

        property string name: ""
        property bool fromRemote: false

        Drag.active: panel.dragSource ? panel.dragSource.drag.active : false
    }

    Row {
        id: panes
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: editStrip.top
        anchors.margins: 6
        spacing: 6

        Repeater {
            // 0 is this computer, 1 is the server. Two panes described once:
            // they differ only in which half of the model they read.
            model: 2

            delegate: Item {
                id: side
                required property int index

                readonly property bool remote: side.index === 1
                readonly property var rows: !panel.files ? []
                    : (side.remote ? panel.files.remoteEntries : panel.files.localEntries)
                readonly property string path: !panel.files ? ""
                    : (side.remote ? panel.files.remotePath : panel.files.localPath)

                width: (panes.width - panes.spacing) / 2
                height: panes.height

                // Enter a folder, or transfer a file to the other side. One
                // gesture for both because which one it is depends on the row,
                // not on the user remembering which button to reach for.
                function activate(row) {
                    if (!panel.files || !row) {
                        return
                    }
                    if (row.isDir) {
                        if (side.remote) {
                            panel.files.enterRemote(row.name)
                        } else {
                            panel.files.enterLocal(row.name)
                        }
                        return
                    }
                    if (side.remote) {
                        panel.files.download(row.name)
                    } else {
                        panel.files.upload(row.name)
                    }
                }

                function up() {
                    if (!panel.files) {
                        return
                    }
                    if (side.remote) {
                        panel.files.leaveRemote()
                    } else {
                        panel.files.leaveLocal()
                    }
                }

                function refresh() {
                    if (!panel.files) {
                        return
                    }
                    if (side.remote) {
                        panel.files.refreshRemote()
                    } else {
                        panel.files.refreshLocal()
                    }
                }

                // T73. Opens the row in the user's editor and watches the copy
                // it downloads. Remote side only: a local file is already open
                // to whatever the user wants to open it with.
                function edit(row) {
                    if (panel.files && side.remote && row && !row.isDir) {
                        panel.files.editRemote(row.name)
                    }
                }

                Item {
                    id: head
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 26

                    BannerButton {
                        id: refreshButton
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Refresh")
                        accent: "#7c869e"
                        onClicked: side.refresh()
                    }

                    BannerButton {
                        id: upButton
                        anchors.right: refreshButton.left
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Up")
                        accent: "#7c869e"
                        onClicked: side.up()
                    }

                    BannerButton {
                        id: editButton
                        anchors.right: upButton.left
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        visible: side.remote
                        text: qsTr("Edit")
                        accent: "#7c869e"
                        onClicked: side.edit(side.rows[list.currentIndex])
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.right: editButton.visible ? editButton.left : upButton.left
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        // The path is SERVER-CONTROLLED on the remote side.
                        textFormat: Text.PlainText
                        // Elided in the MIDDLE: the end of a path is the part
                        // that says where you are.
                        elide: Text.ElideMiddle
                        text: (side.remote ? qsTr("Remote") : qsTr("Local")) + "  " + side.path
                        color: "#e6e9f0"
                        font.family: "Cascadia Mono"
                    }
                }

                Rectangle {
                    id: listBox
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: head.bottom
                    anchors.bottom: parent.bottom
                    color: "#0d0f17"
                    border.width: 1
                    border.color: dropTarget.containsDrag ? "#89b4fa" : "#2c3242"
                    radius: 3
                    clip: true

                    ListView {
                        id: list
                        anchors.fill: parent
                        anchors.margins: 2
                        model: side.rows
                        currentIndex: 0
                        // rules/ui.md is keyboard-first: Return opens a folder
                        // or transfers a file, Backspace goes up, F5 re-reads.
                        focus: true
                        keyNavigationEnabled: true
                        Keys.onReturnPressed: side.activate(side.rows[list.currentIndex])
                        Keys.onEnterPressed: side.activate(side.rows[list.currentIndex])
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_Backspace) {
                                side.up()
                                event.accepted = true
                            } else if (event.key === Qt.Key_F5) {
                                side.refresh()
                                event.accepted = true
                            } else if (event.key === Qt.Key_E && side.remote) {
                                // rules/ui.md is keyboard-first: a feature
                                // reachable only by mouse is incomplete work.
                                side.edit(side.rows[list.currentIndex])
                                event.accepted = true
                            }
                        }

                        delegate: Rectangle {
                            id: row
                            required property int index
                            required property var modelData

                            width: list.width
                            height: 19
                            color: list.currentIndex === row.index ? "#243049" : "transparent"

                            Text {
                                id: nameText
                                anchors.left: parent.left
                                anchors.leftMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                width: row.width - sizeText.width - timeText.width - 16
                                // Remote names are hostile input: PlainText so a
                                // name full of markup is a name, not a document.
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                // ls -F's marks, which cost no font coverage:
                                // "/" is a directory and "@" is a symlink.
                                text: row.modelData.name
                                      + (row.modelData.isDir ? "/" : "")
                                      + (row.modelData.isLink ? "@" : "")
                                color: row.modelData.isDir ? "#9ecbff" : "#e6e9f0"
                                font.family: "Cascadia Mono"
                            }

                            Text {
                                id: sizeText
                                anchors.right: timeText.left
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 72
                                horizontalAlignment: Text.AlignRight
                                textFormat: Text.PlainText
                                text: row.modelData.sizeText
                                color: "#7c869e"
                                font.family: "Cascadia Mono"
                            }

                            Text {
                                id: timeText
                                anchors.right: parent.right
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                width: 110
                                horizontalAlignment: Text.AlignRight
                                textFormat: Text.PlainText
                                text: row.modelData.timeText
                                color: "#7c869e"
                                font.family: "Cascadia Mono"
                            }

                            MouseArea {
                                id: rowMouse
                                anchors.fill: parent
                                drag.target: dragProxy
                                onPressed: (mouse) => {
                                    list.currentIndex = row.index
                                    list.forceActiveFocus()
                                    dragProxy.name = row.modelData.name
                                    dragProxy.fromRemote = side.remote
                                    // Placed under the cursor so the drop lands
                                    // where the pointer is; MouseArea then moves
                                    // it by the mouse delta.
                                    const point = rowMouse.mapToItem(panel, mouse.x, mouse.y)
                                    dragProxy.x = point.x
                                    dragProxy.y = point.y
                                    panel.dragSource = rowMouse
                                }
                                onReleased: {
                                    // Unconditional, the way Qt's own Drag
                                    // example does it: MouseArea may already
                                    // have cleared drag.active by the time this
                                    // runs, and guarding on it is how a drop
                                    // silently does nothing. With no drag in
                                    // flight there is no target and drop()
                                    // returns Qt.IgnoreAction.
                                    dragProxy.Drag.drop()
                                    panel.dragSource = null
                                }
                                onDoubleClicked: side.activate(row.modelData)
                            }
                        }
                    }

                    DropArea {
                        id: dropTarget
                        anchors.fill: parent

                        onDropped: (drop) => {
                            if (!panel.files) {
                                return
                            }
                            if (drop.hasUrls) {
                                // From Explorer. Only the server side takes
                                // them: dropping a local file on the local pane
                                // would be a copy this panel does not do, and
                                // silently doing nothing is worse than not
                                // accepting the drop.
                                if (side.remote) {
                                    panel.files.uploadUrls(drop.urls)
                                    drop.accept(Qt.CopyAction)
                                }
                                return
                            }
                            const from = drop.source
                            if (!from || !from.name) {
                                return
                            }
                            // A drop onto the pane it came from is a no-op
                            // rather than a transfer onto itself.
                            if (side.remote && !from.fromRemote) {
                                panel.files.upload(from.name)
                                drop.accept(Qt.CopyAction)
                            } else if (!side.remote && from.fromRemote) {
                                panel.files.download(from.name)
                                drop.accept(Qt.CopyAction)
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: side.remote && dropTarget.containsDrag
                        text: qsTr("Drop to upload")
                        color: "#89b4fa"
                        textFormat: Text.PlainText
                    }
                }
            }
        }
    }

    // T73. Every remote file still open in an editor, and a way to stop each
    // one. Always visible while it is non-empty and never behind a toggle: a
    // file still being watched after the user believes they are finished is a
    // surprise upload to a production host, and the only defence against that
    // is being able to see it.
    Column {
        id: editStrip
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        spacing: 2
        visible: panel.files ? panel.files.editing.length > 0 : false
        height: visible ? implicitHeight + 6 : 0

        Repeater {
            model: panel.files ? panel.files.editing : []

            delegate: Item {
                id: watched
                required property var modelData

                width: editStrip.width
                height: 20

                BannerButton {
                    id: stopButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Stop watching")
                    accent: "#f38ba8"
                    // localPath, not name: two remote folders can each hold a
                    // config.yml, and the name would stop the wrong one.
                    onClicked: if (panel.files) panel.files.stopEditing(watched.modelData.localPath)
                }

                Text {
                    anchors.left: parent.left
                    anchors.right: stopButton.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    // The remote path is SERVER-CONTROLLED, and it is the whole
                    // point of the line: the user has to be able to see which
                    // machine a save is going to reach.
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    text: qsTr("Editing %1 — saving uploads it back")
                          .arg(watched.modelData.remotePath)
                    color: "#f9e2af"
                    font.family: "Cascadia Mono"
                }
            }
        }
    }

    Item {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 6
        height: 26

        BannerButton {
            id: closeButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Close")
            accent: "#7c869e"
            onClicked: panel.closeRequested()
        }

        BannerButton {
            id: cancelButton
            anchors.right: closeButton.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            visible: panel.files ? panel.files.busy : false
            text: qsTr("Stop")
            accent: "#f38ba8"
            onClicked: if (panel.files) panel.files.cancel()
        }

        // The bar is drawn only when the server said how big the file is.
        // A bar that invents a length lies about how long the wait is.
        Rectangle {
            id: track
            anchors.right: cancelButton.visible ? cancelButton.left : closeButton.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 160
            height: 4
            radius: 2
            color: "#2c3242"
            visible: panel.files ? panel.files.progress >= 0 : false

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: parent.width * (panel.files ? Math.min(1, panel.files.progress) : 0)
                radius: 2
                color: "#a6e3a1"
            }
        }

        Row {
            id: shellButtons
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            visible: panel.files ? panel.files.available : false

            BannerButton {
                text: qsTr("Shell integration")
                accent: "#89b4fa"
                onClicked: if (panel.files) panel.files.proposeShellIntegration(false)
            }

            BannerButton {
                text: qsTr("Uninstall")
                accent: "#7c869e"
                onClicked: if (panel.files) panel.files.proposeShellIntegration(true)
            }
        }

        Text {
            anchors.left: shellButtons.visible ? shellButtons.right : parent.left
            anchors.leftMargin: shellButtons.visible ? 8 : 0
            anchors.right: track.visible ? track.left : cancelButton.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            text: panel.files && panel.files.activity.length > 0
                  ? panel.files.activity
                  : qsTr("Drag a file across to transfer it.")
            color: "#7c869e"
        }
    }

    // T73. The confirmation for a write to SOMEONE ELSE'S MACHINE.
    //
    // Inside the tab, never an app-modal dialog (rules/ui.md) — but it does
    // cover the panel while it is up, because what host, what path and what
    // will be written is the whole of what there is to read at that moment, and
    // a preview squeezed into a corner is one nobody reads before saying yes.
    Rectangle {
        id: installSheet
        anchors.fill: parent
        color: "#12141c"
        visible: panel.files ? panel.files.installStage.length > 0 : false

        readonly property string stage: panel.files ? panel.files.installStage : ""

        // Swallows clicks so the panes underneath cannot be driven while a
        // question about someone else's machine is open.
        MouseArea {
            anchors.fill: parent
        }

        Text {
            id: installTitle
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 10
            wrapMode: Text.WordWrap
            // The host name comes from the profile, but the path is composed
            // from a table plus the server's own answer to realpath.
            textFormat: Text.PlainText
            text: panel.files ? panel.files.installTitle : ""
            color: "#e6e9f0"
        }

        Column {
            id: installChoices
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: installTitle.bottom
            anchors.margins: 10
            spacing: 4
            visible: panel.files ? panel.files.installChoices.length > 0 : false

            Repeater {
                model: panel.files ? panel.files.installChoices : []

                delegate: BannerButton {
                    // Typed, not `var`: installChoices is a QStringList, and
                    // leaving the delegate to convert one at binding time makes
                    // the QML compiler route a plain string assignment through
                    // the JS engine.
                    required property string modelData
                    text: modelData
                    accent: "#89b4fa"
                    onClicked: if (panel.files) panel.files.chooseShellTarget(modelData)
                }
            }
        }

        Text {
            id: previewLabel
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: installChoices.visible ? installChoices.bottom : installTitle.bottom
            anchors.margins: 10
            visible: installSheet.stage === "proposed"
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            text: panel.files && panel.files.installPreview.length > 0
                  ? qsTr("Written between two marker lines in %1. The rest of that file is left exactly as it is.").arg(panel.files.installPath)
                  : qsTr("Krait's block comes out of %1. The rest of that file is left exactly as it is.").arg(panel.files ? panel.files.installPath : "")
            wrapMode: Text.WordWrap
            color: "#7c869e"
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: previewLabel.bottom
            anchors.bottom: installButtons.top
            anchors.margins: 10
            color: "#0d0f17"
            border.width: 1
            border.color: "#2c3242"
            radius: 3
            clip: true
            visible: panel.files ? panel.files.installPreview.length > 0 : false

            Flickable {
                anchors.fill: parent
                anchors.margins: 6
                contentWidth: previewText.width
                contentHeight: previewText.height
                clip: true

                Text {
                    id: previewText
                    // Exactly what goes on the wire, unstyled and unwrapped:
                    // a preview that reflows is not the thing being written.
                    textFormat: Text.PlainText
                    text: panel.files ? panel.files.installPreview : ""
                    color: "#e6e9f0"
                    font.family: "Cascadia Mono"
                }
            }
        }

        Row {
            id: installButtons
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 10
            spacing: 4

            BannerButton {
                visible: installSheet.stage === "proposed"
                text: qsTr("Write the change")
                accent: "#a6e3a1"
                onClicked: if (panel.files) panel.files.confirmShellIntegration()
            }

            BannerButton {
                // Nothing to press while the upload is in flight: there is no
                // "half written" to go back to, and a button that cannot do
                // what it says is worse than no button.
                visible: installSheet.stage !== "writing"
                text: installSheet.stage === "done" ? qsTr("Close") : qsTr("Cancel")
                accent: "#7c869e"
                onClicked: if (panel.files) panel.files.cancelShellIntegration()
            }
        }
    }
}
