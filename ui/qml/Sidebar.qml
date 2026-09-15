import QtQuick
import Isotone

// Sidebar, open (248 px): logo, navigation, working outputs, Settings.
Rectangle {
    id: root
    property string current: "eq"
    signal navigate(string view)

    width: 248
    color: Theme.plot

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.gridMinor
    }

    component NavItem: Rectangle {
        id: item
        property string view
        property string icon
        property string label
        readonly property bool on: root.current === view
        width: parent.width
        height: 38
        radius: 10
        color: on ? Theme.surface : "transparent"
        Row {
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12
            Icon { name: item.icon; size: 18; colour: item.on ? Theme.text : Theme.muted; anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: item.label
                font.family: Theme.font
                font.pixelSize: 14
                color: item.on ? Theme.text : Theme.muted
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.navigate(item.view) }
    }

    Column {
        x: 16
        y: 22
        width: parent.width - 32
        spacing: 0

        Item {
            width: parent.width
            height: 32
            Rectangle {
                id: logo
                x: 10
                width: 28
                height: 28
                radius: 9
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.text
                Icon { name: "logo"; size: 16; strokeWidth: 2.2; colour: Theme.background; anchors.centerIn: parent }
            }
            Text {
                anchors.left: logo.right
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "Isotone"
                font.family: Theme.font
                font.pixelSize: 17
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Item {
                width: 32
                height: 32
                anchors.right: parent.right
                anchors.rightMargin: 4
                Icon { name: "panel"; size: 18; anchors.centerIn: parent }
            }
        }
        Item { width: 1; height: 22 }
        NavItem { view: "eq"; icon: "eq"; label: "Equalizer" }
        NavItem { view: "ear"; icon: "ear"; label: "EQ by ear" }
        NavItem { view: "devices"; icon: "devices"; label: "Devices" }
    }

    Column {
        x: 16
        width: parent.width - 32
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 22
        spacing: 0

        Text {
            leftPadding: 10
            bottomPadding: 8
            text: "Outputs"
            font.family: Theme.font
            font.pixelSize: 12
            color: Theme.muted
        }
        Repeater {
            model: Outputs
            delegate: Rectangle {
                id: row
                required property int index
                required property string name
                required property string backendLabel
                required property string activity
                required property bool current
                width: parent.width
                height: 54
                radius: 10
                color: current ? Theme.surface : "transparent"
                Column {
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Row {
                        spacing: 8
                        Rectangle {
                            width: 6
                            height: 6
                            radius: 3
                            anchors.verticalCenter: parent.verticalCenter
                            color: row.activity === "running" ? Theme.running
                                 : row.activity === "stalled" ? Theme.warning : Theme.muted
                        }
                        Text {
                            text: row.name
                            width: 190
                            elide: Text.ElideRight
                            font.family: Theme.font
                            font.pixelSize: 13
                            color: row.current ? Theme.text : Theme.muted
                        }
                    }
                    Text {
                        leftPadding: 14
                        text: row.backendLabel
                        font.family: Theme.font
                        font.pixelSize: 12
                        color: Theme.muted
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Outputs.select(row.index)
                }
            }
        }
        Item { width: 1; height: 14 }
        Rectangle { width: parent.width; height: 1; color: Theme.gridMinor }
        Item { width: 1; height: 14 }
        NavItem { view: "settings"; icon: "settings"; label: "Settings" }
    }
}
