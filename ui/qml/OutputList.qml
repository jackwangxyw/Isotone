import QtQuick
import Isotone

// The working outputs (the prototype's .orow): status mark, name, backend. Used
// by the collapsed rail's outputs popover, and by OutputPicker for both of its
// states.
//
// `onlyCurrent` keeps the current output's row and drops the rest, which is how
// the picker draws itself shut without a second copy of the row to keep in step.
Column {
    id: root
    // The app's outputs. A property so a test can supply its own: the real
    // singleton enumerates the machine, and under Qt Quick Test that is empty,
    // so nothing here could be covered otherwise.
    property var source: Outputs
    property bool onlyCurrent: false
    signal picked()
    spacing: 2

    Repeater {
        model: root.source
        delegate: Rectangle {
            id: row
            required property int index
            required property string name
            required property string backendLabel
            required property string activity
            required property bool current
            width: root.width
            visible: !root.onlyCurrent || current
            height: visible ? 52 : 0
            radius: 8
            color: current ? Theme.surface : rowArea.containsMouse ? Qt.alpha(Theme.surface, 0.5) : "transparent"
            StatusDot {
                x: 10
                // The mark is 14 tall now, not 6; its centre stays where it was.
                y: 12
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
                // In the picker's shut state the row is a label, not a target:
                // the box around it is what opens. Leaving this enabled gives
                // the click to a row that is already current, so nothing
                // happens and the box never opens.
                enabled: !root.onlyCurrent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: { Outputs.select(row.index); root.picked() }
            }
        }
    }
}
