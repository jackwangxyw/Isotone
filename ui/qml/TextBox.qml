import QtQuick
import Isotone

// The prototype's .txt field: 36 px, radius 8, surface, accent ring on focus.
// With `label`, the .field layout: a 12 px muted label above.
Column {
    id: root
    property string label
    property alias text: input.text
    property alias input: input
    property string placeholder
    signal accepted()

    spacing: 6
    implicitWidth: 240

    Text {
        visible: root.label !== ""
        text: root.label
        font.family: Theme.font
        font.pixelSize: 12
        color: Theme.muted
    }
    Rectangle {
        width: root.width
        height: 36
        radius: 8
        color: Theme.surface
        border.width: input.activeFocus ? 1.5 : 0
        border.color: Theme.accent
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            verticalAlignment: TextInput.AlignVCenter
            font.family: Theme.font
            font.pixelSize: 14
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: Theme.textOnAccent
            selectByMouse: true
            clip: true
            onAccepted: root.accepted()
            Text {
                visible: input.text === "" && root.placeholder !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: root.placeholder
                font: input.font
                color: Theme.muted
            }
        }
    }
}
