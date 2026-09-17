import QtQuick
import Isotone

// Sidebar: open (248 px) with logo, navigation, working outputs and Settings;
// collapsed (72 px) to an icon rail whose Outputs button opens the same list as
// a popover (outputsRequested, in window coordinates).
Rectangle {
    id: root
    readonly property bool open: AppSettings.sidebarOpen
    signal outputsRequested(real x, real y)

    width: open ? 248 : 72
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
        readonly property bool on: UiState.view === view
        objectName: "nav_" + view
        width: AppSettings.sidebarOpen ? parent.width : 44
        height: AppSettings.sidebarOpen ? 38 : 44
        // No anchor: an anchor set for the rail and then removed leaves the item
        // where the rail put it (the sidebar was broken after expanding again).
        radius: AppSettings.sidebarOpen ? 8 : 10
        color: on ? Theme.surface : navArea.containsMouse ? Qt.alpha(Theme.surface, 0.5) : "transparent"
        Row {
            x: AppSettings.sidebarOpen ? 12 : (parent.width - 20) / 2
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12
            Icon { name: item.icon; size: AppSettings.sidebarOpen ? 18 : 20; colour: item.on ? Theme.text : Theme.muted; anchors.verticalCenter: parent.verticalCenter }
            Text {
                visible: AppSettings.sidebarOpen
                text: item.label
                font.family: Theme.font
                font.pixelSize: 14
                color: item.on ? Theme.text : Qt.alpha(Theme.text, 0.82)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea { id: navArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: UiState.view = item.view }
    }

    // Everything scrolls in a window too short for the navigation and the foot
    // (owner, 2026-09-16: a short window).
    Flickable {
        id: flick
        objectName: "sidebarFlick"
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, nav.y + nav.height + 24 + (root.open ? foot.height : rail.height) + 16)
        interactive: contentHeight > height
        clip: interactive
        boundsBehavior: Flickable.StopAtBounds

        // Logo, and the panel toggle (beside the name when open, under the mark when collapsed).
        Rectangle {
            id: logo
            x: root.open ? 26 : (root.width - width) / 2
            y: 24
            width: 28
            height: 28
            radius: 7
            color: Theme.dark ? Theme.knob : "#1b2025"
            Icon { name: "logo"; size: 16; strokeWidth: 2.2; colour: Theme.dark ? "#121519" : "#fcfdff"; anchors.centerIn: parent }
        }
        Text {
            visible: root.open
            anchors.left: logo.right
            anchors.leftMargin: 12
            anchors.verticalCenter: logo.verticalCenter
            text: "Isotone"
            font.family: Theme.font
            font.pixelSize: 17
            font.weight: Font.DemiBold
            font.letterSpacing: -0.17
            color: Theme.text
        }
        Rectangle {
            objectName: "sidebarToggle"
            x: root.open ? root.width - 16 - width : (root.width - width) / 2
            y: root.open ? logo.y : logo.y + logo.height + 16
            width: root.open ? 28 : 44
            height: root.open ? 28 : 44
            radius: root.open ? 6 : 10
            color: toggleArea.containsMouse ? Theme.surface : "transparent"
            Icon { name: "panel"; size: 18; anchors.centerIn: parent }
            MouseArea { id: toggleArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: AppSettings.sidebarOpen = !AppSettings.sidebarOpen }
        }

        Column {
            id: nav
            objectName: "sidebarNav"
            x: root.open ? 16 : 14
            y: root.open ? 80 : 128
            width: parent.width - (root.open ? 32 : 28)
            spacing: root.open ? 2 : 8
            NavItem { view: "eq"; icon: "eq"; label: "Equalizer" }
            NavItem { view: "speakers"; icon: "speakers"; label: "Speakers"; visible: EqSession.outputChannels > 2 }
            NavItem { view: "ear"; icon: "ear"; label: "EQ by ear"; visible: EqByEar.supported }
            NavItem { view: "devices"; icon: "devices"; label: "Devices" }
        }

        // Open: the outputs list and Settings.
        Column {
            id: foot
            objectName: "sidebarFoot"
            visible: root.open
            x: 16
            width: parent.width - 32
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            spacing: 0

            Text {
                leftPadding: 10
                bottomPadding: 8
                text: "Outputs"
                font.family: Theme.font
                font.pixelSize: 13
                font.weight: Font.Medium
                color: Theme.muted
            }
            OutputList { width: parent.width }
            Item { width: 1; height: 10 }
            Rectangle { width: parent.width; height: 1; color: Theme.gridMajor }
            Item { width: 1; height: 12 }
            NavItem { view: "settings"; icon: "settings"; label: "Settings" }
        }

        // Collapsed: the Outputs button with the current output's dot, and Settings.
        Column {
            id: rail
            objectName: "sidebarRail"
            visible: !root.open
            width: 44   // the rail's items; a Column sized by its children moves as they change
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            spacing: 12
            Rectangle {
                id: outputsButton
                objectName: "railOutputs"
                width: 44
                height: 44
                radius: 10
                color: outputsArea.containsMouse ? Theme.surface : "transparent"
                Icon { name: "output"; size: 20; anchors.centerIn: parent }
                StatusDot {
                    x: 34 - 10 + 4
                    y: 11
                    status: Outputs.currentActivity === "running" ? "ok" : Outputs.currentActivity === "stalled" ? "warn" : ""
                }
                MouseArea {
                    id: outputsArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        const p = outputsButton.mapToItem(null, outputsButton.width + 12, 0)
                        root.outputsRequested(p.x, p.y)
                    }
                }
            }
            Rectangle { width: 40; height: 1; anchors.horizontalCenter: parent.horizontalCenter; color: Theme.gridMajor }
            NavItem { view: "settings"; icon: "settings"; label: "Settings" }
        }
    }
}
