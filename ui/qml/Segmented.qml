import QtQuick

// A segmented control: options on a track, the current one raised.
Rectangle {
    id: root
    property var options: []
    property int current: 0
    property int fontSize: 12
    signal picked(int index)

    implicitWidth: row.implicitWidth + 6
    implicitHeight: row.implicitHeight + 6
    radius: 9
    color: Theme.track

    Row {
        id: row
        x: 3
        y: 3
        spacing: 2
        Repeater {
            model: root.options
            delegate: Rectangle {
                required property string modelData
                required property int index
                width: label.implicitWidth + 22
                height: label.implicitHeight + 10
                radius: 6
                color: index === root.current ? Theme.segmentedSelected : "transparent"
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: modelData
                    font.family: Theme.font
                    font.pixelSize: root.fontSize
                    color: index === root.current ? Theme.text : Theme.muted
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.picked(index)
                }
            }
        }
    }
}
