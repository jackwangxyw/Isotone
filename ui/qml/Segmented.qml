import QtQuick

// A segmented control: options on a track, the current one raised. The label is
// the same type as every other value in the app (13, DemiBold), and the raised
// option is centred in the track whatever height the control is given (it used
// to sit 3 px from the top and flush with the bottom).
Rectangle {
    id: root
    property var options: []
    property int current: 0
    property int fontSize: 13
    property int fontWeight: Font.DemiBold
    signal picked(int index)

    implicitWidth: row.implicitWidth + 6
    implicitHeight: row.implicitHeight + 6
    radius: 9
    color: Theme.track

    Row {
        id: row
        anchors.centerIn: parent
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
                    font.weight: root.fontWeight
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
