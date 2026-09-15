import QtQuick

// A band's vertical gain slider: ±12 dB over 132 px, filled from 0 dB.
// Double-click resets to 0 dB.
Item {
    id: root
    property real gain
    property color colour: Theme.accent
    readonly property real range: 12
    signal moved(real gain)
    signal released()

    implicitWidth: 28
    implicitHeight: 132

    readonly property real mid: height / 2
    readonly property real knobY: mid - Math.max(-range, Math.min(range, gain)) / range * mid

    Rectangle { x: 12; width: 4; height: root.height; radius: 2; color: Theme.track }
    Rectangle { x: 8; y: root.mid; width: 12; height: 1; color: Theme.muted }
    Rectangle {
        x: 12
        width: 4
        radius: 2
        color: root.colour
        y: Math.min(root.knobY, root.mid)
        height: Math.abs(root.mid - root.knobY)
    }
    Rectangle {
        x: 5
        y: root.knobY - 9
        width: 18
        height: 18
        radius: 9
        color: Theme.knob
        border.width: 2
        border.color: root.colour
    }
    MouseArea {
        anchors.fill: parent
        anchors.topMargin: -9
        anchors.bottomMargin: -9
        cursorShape: Qt.SizeVerCursor
        preventStealing: true   // the band strip scrolls on drag; this drag is the slider's
        function set(mouseY) {
            const y = Math.max(0, Math.min(root.height, mouseY - 9))
            root.moved(Math.round((root.mid - y) / root.mid * root.range * 10) / 10)
        }
        onPressed: (mouse) => set(mouse.y)
        onPositionChanged: (mouse) => { if (pressed) set(mouse.y) }
        onReleased: root.released()
        onDoubleClicked: { root.moved(0); root.released() }
    }
}
