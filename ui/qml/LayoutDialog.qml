import QtQuick
import Isotone

// "Change <output> to <layout>?" before the Speakers view changes the output's
// Windows speaker setup, or on Linux, Settings changes Isotone's sink's layout.
DialogFrame {
    id: root
    property string outputName
    property string from
    property string to
    property int fromChannels
    property int toChannels
    signal confirmed()

    title: "Change " + outputName + " to " + to + "?"
    cardWidth: 460

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
    // Linux: the layout of Isotone's own sink, from Settings, General.
    DetailRow { label: Qt.platform.os === "linux" ? "Speaker setup" : "Windows speaker setup"; value: root.from + " → " + root.to }
    DetailRow { label: "Channels"; value: root.fromChannels + " → " + root.toChannels }

    actions: [
        Button { objectName: "layoutCancel"; text: "Cancel"; onClicked: root.close() },
        Button {
            objectName: "layoutChange"
            text: "Change"
            kind: "primary"
            onClicked: {
                root.confirmed()
                root.close()
            }
        }
    ]
}
