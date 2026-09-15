import QtQuick
import Isotone

// New group: a name and the speakers in it, for bands to target.
DialogFrame {
    id: root
    property var picked: []   // speaker codes
    readonly property string name: nameBox.text.trim()
    readonly property bool taken: Speakers.groups.some(g => g.name.toLowerCase() === root.name.toLowerCase())

    title: "New group"
    cardWidth: 440

    function toggle(code) {
        const i = picked.indexOf(code)
        picked = i >= 0 ? picked.filter(c => c !== code) : picked.concat([code])
    }

    Item { width: 1; height: 18 }
    TextBox {
        id: nameBox
        objectName: "groupName"
        width: parent.width
        label: "Name"
        Component.onCompleted: input.forceActiveFocus()
    }
    Item { width: 1; height: 16 }
    Text {
        text: "Speakers"
        font.family: Theme.font
        font.pixelSize: 12
        color: Theme.muted
    }
    Item { width: 1; height: 6 }
    Flow {
        width: parent.width
        spacing: 4
        Repeater {
            model: Speakers.speakers
            delegate: Rectangle {
                id: chip
                required property var modelData
                readonly property bool on: root.picked.indexOf(modelData.code) >= 0
                objectName: "groupSpeaker_" + modelData.code
                width: Math.max(44, chipText.implicitWidth + 14)
                height: 30
                radius: 6
                color: on ? Theme.accent : Theme.track
                Text {
                    id: chipText
                    anchors.centerIn: parent
                    text: chip.modelData.code
                    font.family: Theme.font
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: chip.on ? Theme.textOnAccent : Theme.muted
                }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.toggle(chip.modelData.code) }
            }
        }
    }

    actions: [
        Button { objectName: "groupCancel"; text: "Cancel"; onClicked: root.close() },
        Button {
            objectName: "groupCreate"
            text: "Create"
            kind: "primary"
            active: root.name !== "" && !root.taken && root.picked.length > 0
            onClicked: if (Speakers.addGroup(root.name, root.picked)) root.close()
        }
    ]
}
