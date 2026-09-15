import QtQuick
import Isotone

// Keeping Equalizer APO (prototype dialog attach): config.txt with its line
// numbers, the Peace include marked, the lines attaching adds, and Peace Keep or
// Remove include. Attach writes through windows/compat (EqualizerApoConfig).
DialogFrame {
    id: root
    property var device: ({})
    property int peace: 0   // 0 Keep, 1 Remove include
    property string failure
    readonly property var preview: EqualizerApoConfig.preview()
    readonly property bool hasPeace: (preview.lines || []).some((l) => l.peace)
    objectName: "attachDialog"
    cardWidth: 620

    component Line: Rectangle {
        property string number
        property string text
        property color numberColour: Theme.muted
        property string tag
        width: parent.width
        height: 26
        Text {
            width: 40
            rightPadding: 12
            horizontalAlignment: Text.AlignRight
            anchors.verticalCenter: parent.verticalCenter
            text: parent.number
            font.family: Theme.font
            font.pixelSize: 13
            color: parent.numberColour
        }
        Text {
            x: 40
            width: parent.width - 54 - (tagText.visible ? tagText.width + 8 : 0)
            elide: Text.ElideRight
            anchors.verticalCenter: parent.verticalCenter
            text: parent.text
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.text
        }
        Text {
            id: tagText
            visible: parent.tag !== ""
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: parent.tag
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.warning
        }
    }

    Item {
        width: parent.width
        height: 26
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "config.txt"
            font.family: Theme.font
            font.pixelSize: 18
            font.weight: Font.DemiBold
            font.letterSpacing: -0.18
            color: Theme.text
        }
        Text {
            objectName: "attachDirectory"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, parent.width - 120)
            elide: Text.ElideMiddle
            text: root.preview.directory || ""
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.muted
        }
    }
    Item { width: 1; height: 16 }
    Rectangle {
        width: parent.width
        height: lines.implicitHeight + 20
        radius: 10
        color: Theme.plot
        Column {
            id: lines
            objectName: "attachLines"
            y: 10
            width: parent.width
            Repeater {
                model: root.preview.lines || []
                delegate: Line {
                    required property var modelData
                    required property int index
                    objectName: "configLine" + index
                    number: index + 1
                    text: modelData.text
                    tag: modelData.peace ? "Peace" : ""
                    color: modelData.peace ? Qt.alpha(Theme.warning, 0.12) : "transparent"
                }
            }
            Repeater {
                model: root.preview.added || []
                delegate: Line {
                    required property string modelData
                    required property int index
                    objectName: "addedLine" + index
                    number: "+"
                    numberColour: Theme.ok
                    text: modelData
                    color: Qt.alpha(Theme.ok, 0.14)
                }
            }
        }
    }
    Item { width: 1; height: 12; visible: peaceRow.visible }
    Item {
        id: peaceRow
        visible: root.hasPeace
        width: parent.width
        height: 39
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Peace"
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.text
        }
        Segmented {
            objectName: "peaceChoice"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            options: ["Keep", "Remove include"]
            current: root.peace
            onPicked: (index) => root.peace = index
        }
    }
    Text {
        objectName: "attachFailure"
        visible: text !== ""
        width: parent.width
        topPadding: 12
        wrapMode: Text.Wrap
        text: root.failure !== "" ? root.failure : (root.preview.error || "")
        font.family: Theme.font
        font.pixelSize: 13
        color: Theme.danger
    }

    actions: [
        Button { objectName: "attachCancel"; text: "Cancel"; onClicked: root.close() },
        Button {
            objectName: "attachConfirm"
            text: "Attach"
            kind: "primary"
            active: !root.preview.error
            onClicked: {
                root.failure = EqualizerApoConfig.attach(root.hasPeace && root.peace === 1)
                if (root.failure !== "") return
                Devices.refresh()
                root.close()
            }
        }
    ]
}
