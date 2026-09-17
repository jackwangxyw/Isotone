import QtQuick
import Isotone

// "Turn your volume down", before EQ by ear's first tone of the session.
DialogFrame {
    id: root
    signal confirmed()

    title: "Turn your volume down"
    cardWidth: 420

    component DetailRow: Item {
        id: detail
        property string label
        property string value
        width: parent ? parent.width : 0
        height: 39
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: detail.label
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.muted
        }
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: detail.value
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.text
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.gridMinor }
    }

    Item { width: 1; height: 16 }
    DetailRow { label: "Tone level"; value: Theme.dbfs(EqByEar.levelDb) }
    DetailRow { label: "Output"; value: Outputs.currentName }

    actions: [
        Button { objectName: "volumeCancel"; text: "Cancel"; onClicked: root.close() },
        Button {
            objectName: "volumeStart"
            text: "Start"
            kind: "primary"
            onClicked: {
                root.confirmed()
                root.close()
            }
        }
    ]
}
