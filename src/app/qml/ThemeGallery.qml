import QtQuick
import QtQuick.Dialogs
import Krait

// The theme gallery and the live editor (plan T77), in one overlay.
//
// One surface rather than two, because they are one task: you pick a theme by
// looking at it, and the moment you want it slightly different you are already
// looking at the thing you would edit. A separate editor page would mean
// picking, closing, opening something else, and losing the comparison.
//
// rules/ui.md: views only. Every decision — what the list holds, what a token
// is called, whether a colour is legal, where the file goes — is in ThemeModel
// and the pure code behind it. This file positions rectangles and forwards keys.
//
// Like Palette, it is modal to the KEYBOARD and never to the application:
// Escape closes it, output underneath keeps arriving, nothing waits on it.
Item {
    id: gallery

    signal dismissed
    // Raised as a per-tab banner by Main.qml. rules/ui.md bans app-modal
    // surfaces in session flows, so an import that fails says so in a strip.
    signal reported(string message, string detail)

    visible: false
    anchors.fill: parent

    // What the editor is called if it is saved. Seeded from the theme being
    // edited plus a suffix, because a builtin's name is reserved and telling
    // the user that only after they have typed one is worse than offering it.
    property string saveName: ""

    function open() {
        gallery.visible = true
        list.currentIndex = gallery.indexOfCurrent()
        list.forceActiveFocus()
    }

    function close() {
        // An unsaved edit is dropped, and the terminal snaps back. Keeping it
        // alive behind a closed editor is how somebody ends up with colours
        // they cannot find the control for.
        Theme.revert()
        gallery.visible = false
        gallery.dismissed()
    }

    function indexOfCurrent() {
        for (let i = 0; i < Theme.gallery.length; ++i)
            if (Theme.gallery[i].current) return i
        return 0
    }

    function applyCurrent() {
        const entry = Theme.gallery[list.currentIndex]
        if (!entry) return
        Theme.apply(entry.name)
        // Seeded, not forced: the user can overwrite it, and it only matters
        // once they edit something.
        if (gallery.saveName.length === 0)
            gallery.saveName = entry.name + qsTr(" copy")
    }

    function saveEdited() {
        const wanted = gallery.saveName.trim()
        if (wanted.length === 0) {
            gallery.reported(qsTr("Give the theme a name before saving it."), "")
            return
        }
        const error = Theme.saveAs(wanted)
        if (error.length > 0)
            gallery.reported(qsTr("That theme could not be saved."), error)
        else
            gallery.reported(qsTr("Saved %1.").arg(wanted), "")
    }

    function importScheme() {
        picker.open()
    }

    FileDialog {
        id: picker
        title: qsTr("Import a colour scheme")
        nameFilters: [
            qsTr("Colour schemes (*.itermcolors *.json *.yaml *.yml *.toml)"),
            qsTr("All files (*)")
        ]
        onAccepted: {
            // The model takes a filesystem path; a file:// URL reaches QFile as
            // a relative name that does not exist.
            const error = Theme.importFile(picker.selectedFile.toString()
                                                 .replace(/^file:\/\/\//, ""))
            if (error.length > 0)
                gallery.reported(qsTr("That colour scheme could not be imported."), error)
            else
                gallery.reported(qsTr("Imported %1.").arg(Theme.lastImported()), "")
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: gallery.close()
    }

    Rectangle {
        color: Theme.scrim
        opacity: 0.45
        anchors.fill: parent
    }

    Rectangle {
        id: panel
        width: Math.min(parent.width - 80, 860)
        height: Math.min(parent.height - 80, 560)
        anchors.centerIn: parent
        color: Theme.overlay
        border.color: Theme.border
        border.width: 1
        radius: 6

        // Swallows clicks so they do not reach the dismiss area behind.
        MouseArea { anchors.fill: parent }

        Column {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            Row {
                width: parent.width
                spacing: 12

                Text {
                    text: qsTr("Themes")
                    color: Theme.text
                    font.pixelSize: 16
                    font.bold: true
                }
                Text {
                    text: Theme.edited ? qsTr("· edited, not saved") : ""
                    color: Theme.warning
                    font.pixelSize: 13
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // The gallery: one row per theme, showing the palette rather than
            // the name. A theme list that is only names is a list you have to
            // apply every entry of to read.
            ListView {
                id: list
                width: parent.width
                height: parent.height * 0.42
                clip: true
                model: Theme.gallery
                currentIndex: 0
                keyNavigationEnabled: true
                focus: true

                Keys.onReturnPressed: gallery.applyCurrent()
                Keys.onEnterPressed: gallery.applyCurrent()
                Keys.onEscapePressed: gallery.close()
                // Tab moves to the editor rather than out of the overlay:
                // rules/ui.md's keyboard-first rule means every control here has
                // to be reachable without a mouse, and the token list is one.
                Keys.onTabPressed: tokens.forceActiveFocus()

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property var modelData
                    width: list.width
                    height: 44
                    color: list.currentIndex === row.index ? Theme.selection : "transparent"

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            list.currentIndex = row.index
                            gallery.applyCurrent()
                        }
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        spacing: 10

                        // The swatch strip: background, foreground, then the
                        // eight normal ANSI colours. It is what tells two
                        // Solarizeds apart at a glance.
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 0
                            Repeater {
                                model: 10
                                Rectangle {
                                    required property int index
                                    width: 12
                                    height: 20
                                    color: index === 0 ? row.modelData.bg
                                         : index === 1 ? row.modelData.fg
                                         : row.modelData.ansi[index - 1]
                                }
                            }
                        }

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 1
                            Text {
                                text: row.modelData.name
                                color: Theme.text
                                font.pixelSize: 13
                                font.bold: row.modelData.current
                            }
                            Text {
                                text: row.modelData.builtin
                                      ? (row.modelData.dark ? qsTr("Built in · dark")
                                                            : qsTr("Built in · light"))
                                      : row.modelData.source
                                color: Theme.textFaint
                                font.pixelSize: 11
                                elide: Text.ElideMiddle
                                width: Math.max(0, panel.width - 260)
                            }
                        }
                    }

                    Text {
                        visible: row.modelData.current
                        text: qsTr("in use")
                        color: Theme.accent
                        font.pixelSize: 11
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            // The live editor. Every keystroke repaints, because the preview IS
            // the current theme in the store — there is no second rendering
            // path that could disagree with the real one.
            ListView {
                id: tokens
                width: parent.width
                height: parent.height * 0.36
                clip: true
                model: Theme.tokens
                keyNavigationEnabled: true

                Keys.onEscapePressed: gallery.close()
                Keys.onBacktabPressed: list.forceActiveFocus()

                section.property: "group"
                section.delegate: Text {
                    required property string section
                    text: section
                    color: Theme.textFaint
                    font.pixelSize: 11
                    topPadding: 6
                    bottomPadding: 2
                }

                delegate: Row {
                    id: token
                    required property var modelData
                    width: tokens.width
                    height: 26
                    spacing: 10

                    Rectangle {
                        width: 22
                        height: 18
                        anchors.verticalCenter: parent.verticalCenter
                        color: token.modelData.value
                        border.color: Theme.border
                        border.width: 1
                    }
                    Text {
                        text: token.modelData.label
                        color: Theme.text
                        font.pixelSize: 12
                        width: 170
                        elide: Text.ElideRight
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: 100
                        height: 20
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.surfaceAlt
                        border.color: hex.activeFocus ? Theme.accent : Theme.border
                        border.width: 1
                        TextInput {
                            id: hex
                            anchors.fill: parent
                            anchors.margins: 3
                            text: token.modelData.hex
                            color: Theme.text
                            font.pixelSize: 12
                            selectByMouse: true
                            // Applied on every edit, not on Enter: "live" is the
                            // feature. A rejected value simply does not apply —
                            // the field keeps what was typed, so a half-finished
                            // "#12" is not erased out from under the user.
                            onTextEdited: Theme.setToken(token.modelData.key, text)
                            Keys.onEscapePressed: gallery.close()
                        }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Row {
                width: parent.width
                spacing: 8

                Rectangle {
                    width: 150
                    height: 24
                    color: Theme.surfaceAlt
                    border.color: nameField.activeFocus ? Theme.accent : Theme.border
                    border.width: 1
                    TextInput {
                        id: nameField
                        anchors.fill: parent
                        anchors.margins: 4
                        text: gallery.saveName
                        color: Theme.text
                        font.pixelSize: 12
                        selectByMouse: true
                        onTextEdited: gallery.saveName = text
                        Keys.onReturnPressed: gallery.saveEdited()
                        Keys.onEscapePressed: gallery.close()
                    }
                    Text {
                        visible: nameField.text.length === 0
                        text: qsTr("Save as…")
                        color: Theme.textFaint
                        font.pixelSize: 12
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 5
                    }
                }

                BannerButton {
                    text: qsTr("Save")
                    accent: Theme.success
                    onClicked: gallery.saveEdited()
                }
                BannerButton {
                    text: qsTr("Revert")
                    accent: Theme.textDim
                    onClicked: Theme.revert()
                }
                BannerButton {
                    text: qsTr("Import…")
                    accent: Theme.accent
                    onClicked: gallery.importScheme()
                }
                BannerButton {
                    text: qsTr("Close")
                    accent: Theme.textDim
                    onClicked: gallery.close()
                }
            }
        }
    }
}
