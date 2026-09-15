import QtQuick
import Isotone

// A Settings row, the prototype's .srow: the label on the left, the control on
// the right, at least 48 px, a hairline under it.
Item {
    id: root
    property string label
    default property alias content: slot.data
    property alias labelItem: labelText

    width: parent ? parent.width : 760
    height: Math.max(48, slot.childrenRect.height + 16)

    Text {
        id: labelText
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        font.family: Theme.font
        font.pixelSize: 14
        color: Theme.text
    }
    Item {
        id: slot
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: childrenRect.width
        height: childrenRect.height
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.gridMinor
    }
}
