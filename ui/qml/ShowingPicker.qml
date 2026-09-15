import QtQuick
import Isotone

// "Showing" in the top bar of a surround output, instead of L / R / L+R: All
// speakers, the groups, then each speaker. What it shows is what the graph draws
// (Speakers.showing). The list opens in the window's overlay, under the picker.
Rectangle {
    id: root
    property Item menu: null
    readonly property bool menuOpen: menu !== null && menu.open

    implicitWidth: row.implicitWidth + 24
    implicitHeight: 32
    radius: 8
    color: area.containsMouse ? Qt.lighter(Theme.surface, Theme.dark ? 1.25 : 0.96) : Theme.surface

    function openMenu() {
        if (!menu) menu = UiState.openDialog(menuComponent, {})
        if (!menu) return
        const p = root.mapToItem(menu, root.width / 2, root.height + 8)
        menu.openAt(p.x - menu.panelWidth / 2, p.y)
    }
    Component.onDestruction: if (menu) menu.destroy()

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Showing"
            font.family: Theme.font
            font.pixelSize: 12
            color: Theme.muted
        }
        Text {
            objectName: "showingLabel"
            anchors.verticalCenter: parent.verticalCenter
            text: Speakers.showingLabel
            font.family: Theme.font
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: Theme.text
        }
        Icon { name: "chevron"; size: 14; anchors.verticalCenter: parent.verticalCenter }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.openMenu()
    }

    Component {
        id: menuComponent
        Popover {
            id: popover
            objectName: "showingMenu"
            panelWidth: 250
            padding: 6
            Column {
                width: parent.width
                Repeater {
                    model: Speakers.showingItems
                    delegate: Column {
                        id: entry
                        required property var modelData
                        width: parent.width
                        Item {
                            visible: entry.modelData.separator
                            width: parent.width
                            height: 9
                            Rectangle { x: 6; y: 4; width: parent.width - 12; height: 1; color: Theme.border }
                        }
                        Rectangle {
                            objectName: "showing_" + entry.modelData.key
                            width: parent.width
                            height: 36
                            radius: 6
                            color: itemArea.containsMouse ? Theme.surface : "transparent"
                            Row {
                                x: 8
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 10
                                Item {
                                    width: 16
                                    height: 16
                                    anchors.verticalCenter: parent.verticalCenter
                                    Icon {
                                        visible: Speakers.showing === entry.modelData.key
                                        name: "check"
                                        size: 15
                                        colour: Theme.accent
                                        anchors.centerIn: parent
                                    }
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entry.modelData.label
                                    font.family: Theme.font
                                    font.pixelSize: 13
                                    color: Theme.text
                                }
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: entry.modelData.detail
                                font.family: Theme.font
                                font.pixelSize: 12
                                color: Theme.muted
                            }
                            MouseArea {
                                id: itemArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    Speakers.showing = entry.modelData.key
                                    popover.close()
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
