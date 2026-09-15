import QtQuick
import Isotone

// The prototype's .spin: 14 px ring, accent arc, 0.9 s a turn.
Item {
    id: root
    implicitWidth: 14
    implicitHeight: 14
    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: "transparent"
        border.width: 2
        border.color: Theme.track
    }
    Canvas {
        id: arc
        anchors.fill: parent
        onPaint: {
            const g = getContext("2d")
            g.reset()
            g.strokeStyle = Theme.accent
            g.lineWidth = 2
            g.beginPath()
            g.arc(width / 2, height / 2, width / 2 - 1, -Math.PI / 2, 0)
            g.stroke()
        }
        RotationAnimation on rotation { from: 0; to: 360; duration: 900; loops: Animation.Infinite; running: root.visible }
    }
}
