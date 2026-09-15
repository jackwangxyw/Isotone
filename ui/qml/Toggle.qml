import QtQuick

// On/off switch: 34 x 20 by default, knob inset 3.
Item {
    id: root
    property bool checked
    property color colour: Theme.accent
    signal toggled(bool on)

    implicitWidth: 34
    implicitHeight: 20

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.checked ? root.colour : Theme.track
    }
    Rectangle {
        readonly property real knob: root.height - 6
        width: knob
        height: knob
        radius: knob / 2
        y: 3
        x: root.checked ? root.width - knob - 3 : 3
        color: root.checked ? Theme.knob : Theme.muted
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled(!root.checked)
    }
}
