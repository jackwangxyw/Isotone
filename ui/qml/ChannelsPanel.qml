import QtQuick
import Isotone

// Channels panel (stereo): open (240 px) with balance and mute; collapsed
// (52 px) to a strip with the vertical label and the balance value.
Item {
    id: root
    readonly property bool open: AppSettings.panelOpen

    width: open ? 240 : 52

    Rectangle { width: 1; height: parent.height; color: Theme.gridMinor }

    // Collapsed.
    Item {
        visible: !root.open
        anchors.fill: parent
        Rectangle {
            objectName: "panelExpand"
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
            text: "Channels"
            font.family: Theme.font
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Theme.text
        }
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 22
            spacing: 2
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Bal"
                font.family: Theme.font
                font.pixelSize: 11
                color: Theme.muted
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: EqSession.balance === 0 ? "0.0" : Theme.signed(EqSession.balance, 1)
                font.family: Theme.font
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Theme.text
            }
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
                text: "Channels"
                font.family: Theme.font
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Rectangle {
                objectName: "panelCollapse"
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
            spacing: 10
            Item {
                width: parent.width
                height: value.height
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Balance"
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.text
                }
                Rectangle {
                    id: value
                    anchors.right: parent.right
                    width: valueText.implicitWidth + 20
                    height: valueText.implicitHeight + 8
                    radius: 7
                    color: Theme.track
                    ValueField {
                        id: valueText
                        objectName: "balanceValue"
                        anchors.centerIn: parent
                        text: EqSession.balance === 0 ? "0.0" : Theme.signed(EqSession.balance, 1)
                        unit: EqSession.Plain
                        weight: Font.DemiBold
                        onSubmitted: (v) => { EqSession.balance = v; EqSession.finishEdit() }
                    }
                }
            }
            // −1.0 to +1.0, filled from the centre.
            Item {
                id: balance
                width: parent.width
                height: 18
                readonly property real centre: width / 2
                readonly property real knobX: centre + EqSession.balance * centre
                Rectangle { y: 7; width: parent.width; height: 4; radius: 2; color: Theme.track }
                Rectangle { x: balance.centre - 1; y: 3; width: 2; height: 12; radius: 1; color: Theme.zero }
                Rectangle {
                    y: 7
                    height: 4
                    radius: 2
                    color: Theme.accent
                    x: Math.min(balance.knobX, balance.centre)
                    width: Math.abs(balance.knobX - balance.centre)
                }
                Rectangle {
                    x: balance.knobX - 9
                    width: 18
                    height: 18
                    radius: 9
                    color: Theme.knob
                    border.width: 2
                    border.color: Theme.accent
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    function set(mouseX) { EqSession.balance = ((mouseX - 6) - balance.centre) / balance.centre }
                    onPressed: (mouse) => set(mouse.x)
                    onPositionChanged: (mouse) => { if (pressed) set(mouse.x) }
                    onReleased: EqSession.finishEdit()
                }
            }
        }

        Item {
            width: parent.width
            height: 20
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Mute"
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.text
            }
            Toggle {
                anchors.right: parent.right
                checked: EqSession.muted
                onToggled: (on) => EqSession.muted = on
            }
        }
    }
}
