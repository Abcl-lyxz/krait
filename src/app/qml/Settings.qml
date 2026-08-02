import QtQuick
import Krait

// The settings page, generated entirely from the schema.
//
// rules/ui.md: one declaration in schema.cpp becomes the TOML key, the search
// entry and this editor. There is no per-setting QML here on purpose — a page
// with a hand-written control per setting is a page that will eventually
// disagree with its own file, and that disagreement always shows up as "I
// changed it and nothing happened".
//
// So the delegate below switches on TYPE, not on id. Adding a setting means
// touching schema.cpp and nothing else; adding a TYPE is the expensive change,
// which is exactly the bar the schema comment sets.
Item {
    id: page

    signal dismissed

    visible: false
    anchors.fill: parent

    function open() {
        settings.query = ""
        field.text = ""
        page.visible = true
        field.forceActiveFocus()
    }

    function close() {
        settings.save()
        page.visible = false
        page.dismissed()
    }

    SettingsModel {
        id: settings
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        Row {
            width: parent.width
            spacing: 12

            Text {
                color: Theme.text
                font.pixelSize: 18
                text: qsTr("Settings")
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 200
                color: Theme.textFaint
                text: settings.path
                elide: Text.ElideMiddle
            }
        }

        // The file was written by a newer Krait. Saying so beats saving and
        // silently dropping whatever that version added.
        Text {
            width: parent.width
            visible: settings.readOnly
            color: Theme.warning
            wrapMode: Text.Wrap
            text: qsTr("This file was written by a newer version of Krait, so changes will not be saved. Update Krait, or edit the file by hand.")
        }

        TextInput {
            id: field
            width: parent.width
            color: Theme.text
            font.pixelSize: 15
            selectByMouse: true
            onTextChanged: settings.query = text
            Keys.onEscapePressed: page.close()

            Text {
                anchors.fill: parent
                visible: field.text.length === 0
                color: Theme.textFaint
                font: field.font
                // Thai ships as a first-class locale and the schema carries
                // Thai keywords, so searching in Thai genuinely works.
                text: qsTr("Search settings…")
            }
        }

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.border
        }

        ListView {
            width: parent.width
            height: parent.height - 120
            clip: true
            spacing: 6
            model: settings.rows

            delegate: Column {
                id: row
                required property var modelData

                width: ListView.view.width
                spacing: 2

                Text {
                    color: Theme.text
                    font.pixelSize: 14
                    text: row.modelData.id
                }

                Text {
                    width: row.width
                    color: Theme.textFaint
                    wrapMode: Text.Wrap
                    text: row.modelData.doc
                }

                // A toggle for a bool, a choice row for an enum, a text field
                // for everything else. Switching on the TYPE is what keeps this
                // file from growing a branch per setting.
                Loader {
                    property var rowData: row.modelData

                    sourceComponent: row.modelData.type === "bool"
                        ? boolEditor
                        : (row.modelData.choices.length > 0 ? choiceEditor : textEditor)
                }
            }
        }
    }

    Component {
        id: boolEditor
        Text {
            color: Theme.accent
            text: rowData.value ? qsTr("on") : qsTr("off")
            MouseArea {
                anchors.fill: parent
                onClicked: settings.setValue(rowData.id, !rowData.value)
            }
        }
    }

    Component {
        id: choiceEditor
        Row {
            spacing: 8
            Repeater {
                model: rowData.choices
                delegate: Text {
                    required property string modelData
                    color: modelData === String(rowData.value) ? Theme.accent : Theme.textFaint
                    text: modelData
                    MouseArea {
                        anchors.fill: parent
                        onClicked: settings.setValue(rowData.id, parent.modelData)
                    }
                }
            }
        }
    }

    Component {
        id: textEditor
        TextInput {
            color: Theme.accent
            text: String(rowData.value)
            selectByMouse: true
            // On editing finished, not on every keystroke: the registry
            // validates and REFUSES rather than clamping, and rejecting a value
            // half-typed would make a legal one impossible to reach.
            onEditingFinished: settings.setValue(rowData.id, text)
        }
    }
}
