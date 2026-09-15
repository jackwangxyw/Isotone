import QtQuick
import Isotone

// Speakers panel (surround): open (240 px) with Layout, Crossover, Upmix and Lip
// sync, Speaker setup, and Mute; collapsed (52 px) to a strip with the vertical
// label and the layout.
Item {
    id: root
    readonly property bool open: AppSettings.panelOpen
    readonly property var upmixNames: ["Off", "All", "No centre"]

    width: open ? 240 : 52

    component InfoRow: Item {
        property string label
        property string value
        width: parent ? parent.width : 0
        height: 30
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: parent.label
            font.family: Theme.font
            font.pixelSize: 13
            color: Qt.alpha(Theme.text, 0.8)
        }
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: parent.value
            font.family: Theme.font
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Theme.text
        }
    }

    Rectangle { width: 1; height: parent.height; color: Theme.gridMinor }

    // Collapsed.
    Item {
        visible: !root.open
        anchors.fill: parent
        Rectangle {
            objectName: "speakersPanelExpand"
            anchors.horizontalCenter: parent.horizontalCenter
            y: 12
            width: 32
            height: 32
            radius: 8
            color: expandArea.containsMouse ? Theme.surface : "transparent"
            Icon { name: "chevronLeft"; size: 16; anchors.centerIn: parent }
            MouseArea { id: expandArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: AppSettings.panelOpen = true }
        }
        Text {
            y: 66 + width
            rotation: -90
            transformOrigin: Item.TopLeft
            x: (parent.width - height) / 2
            text: "Speakers"
            font.family: Theme.font
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Theme.text
        }
        Text {
            objectName: "collapsedLayout"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 22
            text: Speakers.layoutName
            font.family: Theme.font
            font.pixelSize: 11
            color: Theme.muted
        }
    }

    // Open.
    Column {
        visible: root.open
        x: 22
        y: 4
        width: parent.width - 22 - 32
        spacing: 18

        Item {
            width: parent.width
            height: 32
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Speakers"
                font.family: Theme.font
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Rectangle {
                objectName: "speakersPanelCollapse"
                anchors.right: parent.right
                anchors.rightMargin: -8
                width: 32
                height: 32
                radius: 8
                color: collapseArea.containsMouse ? Theme.surface : "transparent"
                Icon { name: "chevronRight"; size: 16; anchors.centerIn: parent }
                MouseArea { id: collapseArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: AppSettings.panelOpen = false }
            }
        }

        Column {
            width: parent.width
            InfoRow { objectName: "layoutRow"; label: "Layout"; value: Speakers.layoutName }
            InfoRow { objectName: "crossoverRow"; label: "Crossover"; value: Speakers.bassManagement ? Math.round(Speakers.crossoverHz) + " Hz" : "Off" }
            InfoRow { objectName: "upmixRow"; label: "Upmix"; value: root.upmixNames[Speakers.upmix] ?? "" }
            InfoRow { objectName: "lipSyncRow"; label: "Lip sync"; value: Math.round(Speakers.lipSyncMs) + " ms" }
            Item { width: 1; height: 12 }
            Rectangle {
                objectName: "speakerSetup"
                width: parent.width
                height: 34
                radius: 8
                color: setupArea.containsMouse ? Qt.lighter(Theme.surface, Theme.dark ? 1.25 : 0.96) : Theme.surface
                Text {
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Speaker setup"
                    font.family: Theme.font
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: Theme.text
                }
                Icon { name: "chevronRight"; size: 14; anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter }
                MouseArea { id: setupArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: UiState.view = "speakers" }
            }
            Item { width: 1; height: 16 }
            Item {
                width: parent.width
                height: 30
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Mute"
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.text
                }
                Toggle {
                    objectName: "speakersMute"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    checked: EqSession.muted
                    onToggled: (on) => EqSession.muted = on
                }
            }
        }
    }
}
