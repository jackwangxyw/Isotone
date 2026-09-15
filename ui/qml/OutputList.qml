import QtQuick
import Isotone

// The working outputs (the prototype's .orow): status dot, name, backend. Used
// by the open sidebar and by the collapsed rail's outputs popover.
Column {
    id: root
    signal picked()
    spacing: 2

    Repeater {
        model: Outputs
        delegate: Rectangle {
            id: row
            required property int index
            required property string name
            required property string backendLabel
            required property string activity
            required property bool current
            width: root.width
            height: 52
            radius: 8
            color: current ? Theme.surface : rowArea.containsMouse ? Qt.alpha(Theme.surface, 0.5) : "transparent"
            StatusDot {
                x: 10
                y: 16
                status: row.activity === "running" ? "ok" : row.activity === "stalled" ? "warn" : ""
            }
            Column {
                x: 24
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 34
                spacing: 2
                Text {
                    width: parent.width
                    text: row.name
                    elide: Text.ElideRight
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: row.current ? Theme.text : Qt.alpha(Theme.text, 0.78)
                }
                Text {
                    text: row.backendLabel
                    font.family: Theme.font
                    font.pixelSize: 12
                    color: Theme.muted
                }
            }
            MouseArea {
                id: rowArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: { Outputs.select(row.index); root.picked() }
            }
        }
    }
}
