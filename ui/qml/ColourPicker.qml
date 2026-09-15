import QtQuick
import Isotone

// A colour: saturation and value square, hue slider and hex field (the
// prototype's Custom accent popover). Every change is picked(colour); set
// `colour` before showing it.
Column {
    id: root
    property color colour: "#2f8cff"
    signal picked(color colour)

    property real hue: 0
    property real saturation: 0
    property real value: 0

    spacing: 10
    width: 230

    function load(source) {
        const c = Qt.color(source)
        colour = c
        // Grey has no hue: keep the one the slider holds.
        if (c.hsvHue >= 0) hue = c.hsvHue
        saturation = c.hsvSaturation
        root.value = c.hsvValue
        hex.text = c.toString().toUpperCase()
    }
    function pick() {
        colour = Qt.hsva(hue, saturation, value, 1)
        hex.text = colour.toString().toUpperCase()
        root.picked(colour)
    }
    // "#2f8cff" or "2F8CFF", as a colour; undefined when the text is not one.
    function parseHex(text) {
        const m = /^\s*#?([0-9a-fA-F]{6})\s*$/.exec(text)
        return m ? Qt.color("#" + m[1].toLowerCase()) : undefined
    }

    Rectangle {
        id: square
        objectName: "saturationValue"
        width: parent.width
        height: 120
        radius: 8
        color: Qt.hsva(root.hue, 1, 1, 1)
        Rectangle {
            anchors.fill: parent
            radius: 8
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "#ffffffff" }
                GradientStop { position: 1; color: "#00ffffff" }
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: 8
            gradient: Gradient {
                GradientStop { position: 0; color: "#00000000" }
                GradientStop { position: 1; color: "#ff000000" }
            }
        }
        Rectangle {
            x: root.saturation * square.width - 7
            y: (1 - root.value) * square.height - 7
            width: 14
            height: 14
            radius: 7
            color: "transparent"
            border.width: 2
            border.color: "white"
        }
        MouseArea {
            anchors.fill: parent
            function set(mouse) {
                root.saturation = Math.max(0, Math.min(1, mouse.x / square.width))
                root.value = Math.max(0, Math.min(1, 1 - mouse.y / square.height))
                root.pick()
            }
            onPressed: (mouse) => set(mouse)
            onPositionChanged: (mouse) => { if (pressed) set(mouse) }
        }
    }

    Rectangle {
        id: hueBar
        objectName: "hue"
        width: parent.width
        height: 12
        radius: 6
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0 / 6; color: "#ff0000" }
            GradientStop { position: 1 / 6; color: "#ffff00" }
            GradientStop { position: 2 / 6; color: "#00ff00" }
            GradientStop { position: 3 / 6; color: "#00ffff" }
            GradientStop { position: 4 / 6; color: "#0000ff" }
            GradientStop { position: 5 / 6; color: "#ff00ff" }
            GradientStop { position: 6 / 6; color: "#ff0000" }
        }
        Rectangle {
            x: root.hue * hueBar.width - 7
            y: -1
            width: 14
            height: 14
            radius: 7
            color: Qt.hsva(root.hue, 1, 1, 1)
            border.width: 2
            border.color: "white"
        }
        MouseArea {
            anchors.fill: parent
            anchors.margins: -4
            function set(mouse) {
                root.hue = Math.max(0, Math.min(1, (mouse.x - 4) / hueBar.width))
                root.pick()
            }
            onPressed: (mouse) => set(mouse)
            onPositionChanged: (mouse) => { if (pressed) set(mouse) }
        }
    }

    TextBox {
        id: hex
        objectName: "hex"
        width: parent.width
        onAccepted: {
            const c = root.parseHex(text)
            if (c === undefined) { input.selectAll(); return }
            root.load(c)
            root.picked(c)
        }
    }
}
