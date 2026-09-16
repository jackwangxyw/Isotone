import QtQuick
import Isotone

// A Settings value on a slider: the track, then the value in the same type as
// every other setting's value (Theme.font, 13, DemiBold). Drag or click the
// track; `text` formats the value, `live` is set while dragging so a page can
// hold off saving.
Row {
    id: root
    property real from: 0
    property real to: 1
    property real value: 0
    property real step: 0
    property int valueWidth: 64
    property var format: (v) => v.toFixed(2)
    readonly property bool dragging: area.pressed
    signal moved(real value)

    spacing: 12

    function quantise(v) {
        const held = Math.max(root.from, Math.min(root.to, v))
        return root.step > 0 ? Math.round(held / root.step) * root.step : held
    }

    Item {
        id: track
        objectName: "sliderTrack"
        width: 160
        height: 18
        anchors.verticalCenter: parent.verticalCenter
        readonly property real t: root.to > root.from ? (root.value - root.from) / (root.to - root.from) : 0

        Rectangle { y: 7; width: parent.width; height: 4; radius: 2; color: Theme.track }
        Rectangle {
            y: 7
            width: track.t * track.width
            height: 4
            radius: 2
            color: Theme.accent
        }
        Rectangle {
            width: 16
            height: 16
            radius: 8
            y: 1
            x: track.t * track.width - 8
            color: Theme.knob
            border.color: Theme.accent
            border.width: 2.5
        }
        MouseArea {
            id: area
            objectName: "sliderArea"
            anchors.fill: parent
            anchors.margins: -6
            cursorShape: Qt.PointingHandCursor
            function set(mouseX) {
                const t = Math.max(0, Math.min(1, (mouseX - 6) / track.width))
                root.moved(root.quantise(root.from + t * (root.to - root.from)))
            }
            onPressed: (mouse) => set(mouse.x)
            onPositionChanged: (mouse) => { if (pressed) set(mouse.x) }
        }
    }
    Text {
        objectName: "sliderValue"
        // A fixed width, right aligned, so the row ends where every other setting's
        // control does and the value does not shift as it changes.
        width: root.valueWidth
        horizontalAlignment: Text.AlignRight
        anchors.verticalCenter: parent.verticalCenter
        text: root.format(root.value)
        font.family: Theme.font
        font.pixelSize: 13
        font.weight: Font.DemiBold
        color: Theme.text
    }
}
