import QtQuick
import Isotone

// Keys as key caps, the prototype's .key: 24 px, radius 5, a hairline and a
// 1 px edge under it, 12 px 600.
Row {
    id: root
    property var keys: []
    property color colour: Theme.text
    property color edge: Theme.gridMajor

    spacing: 4

    Repeater {
        model: root.keys
        delegate: Item {
            required property string modelData
            width: Math.max(26, capText.implicitWidth + 14)
            height: 25
            Rectangle {
                y: 1
                width: parent.width
                height: 24
                radius: 5
                color: root.edge
            }
            Rectangle {
                width: parent.width
                height: 24
                radius: 5
                color: Theme.background
                border.width: 1
                border.color: root.edge
                Text {
                    id: capText
                    anchors.centerIn: parent
                    text: modelData
                    font.family: Theme.font
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: root.colour
                }
            }
        }
    }
}
