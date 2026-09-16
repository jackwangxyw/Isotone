import QtQuick
import Isotone

// Save as: a name for the current output's EQ as a new preset, and what it is
// for: every output, or this one alone (owner, 2026-09-15). A name already taken
// gets a number (Presets.saveAs).
DialogFrame {
    id: root
    signal accepted(string name, string forOutput)

    title: "Save as"
    cardWidth: 440

    // Every output, then each of them: a preset is for all of them unless it is
    // narrowed (owner, 2026-09-15).
    readonly property var choices: [{ guid: "", name: "All outputs" }].concat(Presets.outputChoices())
    property string forOutput: ""
    readonly property string forName: {
        for (const c of choices)
            if (c.guid === forOutput) return c.name
        return "All outputs"
    }

    function accept() {
        const name = nameField.text.trim()
        if (name === "") return
        root.accepted(name, root.forOutput)
        root.close()
    }

    Item { width: 1; height: 18 }
    TextBox {
        id: nameField
        objectName: "saveAsName"
        width: parent.width
        label: "Name"
        text: Presets.untitled ? "" : Presets.currentName
        onAccepted: root.accept()
        // After DialogFrame gives its card the focus.
        Component.onCompleted: Qt.callLater(() => { input.forceActiveFocus(); input.selectAll() })
    }

    Item { width: 1; height: 16; visible: scope.visible }
    Column {
        id: scope
        objectName: "saveAsScope"
        visible: root.choices.length > 1
        width: parent.width
        spacing: 6
        z: 2
        Text {
            text: "For"
            font.family: Theme.font
            font.pixelSize: 12
            color: Theme.muted
        }
        Rectangle {
            id: forBox
            objectName: "saveAsFor"
            width: parent.width
            height: 36
            radius: 8
            color: Theme.surface
            Text {
                x: 12
                width: parent.width - 44
                anchors.verticalCenter: parent.verticalCenter
                text: root.forName
                elide: Text.ElideRight
                font.family: Theme.font
                font.pixelSize: 14
                color: Theme.text
            }
            Icon {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron"
                size: 15
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: forList.visible = !forList.visible
            }
        }
        // Under the box, so the card grows for it and nothing above is covered.
        Rectangle {
            id: forList
            objectName: "saveAsForList"
            visible: false
            width: parent.width
            height: forRows.implicitHeight + 8
            radius: 10
            color: Theme.pop
            border.color: Theme.border
            Column {
                id: forRows
                x: 4
                y: 4
                width: parent.width - 8
                Repeater {
                    model: root.choices
                    delegate: Rectangle {
                        required property var modelData
                        width: forRows.width
                        height: 34
                        radius: 6
                        color: forArea.containsMouse ? Theme.surface : "transparent"
                        Icon {
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            visible: modelData.guid === root.forOutput
                            name: "check"
                            size: 14
                            colour: Theme.accent
                        }
                        Text {
                            x: 30
                            width: parent.width - 40
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            elide: Text.ElideRight
                            font.family: Theme.font
                            font.pixelSize: 13
                            color: Theme.text
                        }
                        MouseArea {
                            id: forArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.forOutput = modelData.guid
                                forList.visible = false
                            }
                        }
                    }
                }
            }
        }
    }

    actions: [
        Button {
            objectName: "saveAsCancel"
            text: "Cancel"
            onClicked: { root.rejected(); root.close() }
        },
        Button {
            objectName: "saveAsSave"
            text: "Save"
            kind: "primary"
            active: nameField.text.trim() !== ""
            onClicked: root.accept()
        }
    ]
}
