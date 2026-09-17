import QtQuick
import Isotone

// EQ by ear's frequency slider: 20 Hz to 20 kHz on a log scale, the marks as
// ticks above it and the frequencies under it. The track's top is at y 7.
Item {
    id: root
    readonly property real low: 20
    readonly property real high: 20000
    function position(hz) { return Math.log(Math.max(low, Math.min(high, hz)) / low) / Math.log(high / low) * width }
    function frequencyAt(x) { return low * Math.pow(high / low, Math.max(0, Math.min(1, x / width))) }

    height: 46

    Rectangle { y: 7; width: parent.width; height: 4; radius: 2; color: Theme.track }
    Rectangle { y: 7; width: root.position(EqByEar.frequency); height: 4; radius: 2; color: Theme.accent }

    Repeater {
        model: [{ letter: "S", hz: EqByEar.start }, { letter: "T", hz: EqByEar.top }, { letter: "E", hz: EqByEar.end }]
        delegate: Item {
            required property var modelData
            visible: modelData.hz > 0
            x: Math.round(root.position(modelData.hz))
            Rectangle { x: -1; y: -5; width: 2; height: 28; color: Theme.muted }
            Text {
                x: -width / 2
                y: -23
                text: parent.modelData.letter
                font.family: Theme.font
                font.pixelSize: 11
                font.weight: Font.DemiBold
                color: Theme.muted
            }
        }
    }

    Rectangle {
        objectName: "sweepKnob"
        width: 20
        height: 20
        radius: 10
        y: -1
        x: root.position(EqByEar.frequency) - 10
        color: Theme.knob
        border.color: Theme.accent
        border.width: 3
    }

    Repeater {
        model: [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
        delegate: Text {
            required property int modelData
            x: Math.round(root.position(modelData) - width / 2)
            y: 29
            text: modelData >= 1000 ? modelData / 1000 + "k" : modelData
            font.family: Theme.font
            font.pixelSize: 11
            color: Theme.muted
        }
    }

    MouseArea {
        objectName: "sweepArea"
        // The knob's half past either end.
        x: -10
        y: -3
        width: parent.width + 20
        height: 24
        cursorShape: Qt.SizeHorCursor
        preventStealing: true
        onPressed: (mouse) => EqByEar.frequency = root.frequencyAt(mouse.x - 10)
        onPositionChanged: (mouse) => { if (pressed) EqByEar.frequency = root.frequencyAt(mouse.x - 10) }
    }
}
